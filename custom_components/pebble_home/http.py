"""HTTP API used by the Pebble's phone-side JavaScript.

Authentication is a per-install key (header ``X-Pebble-Key``), not a Home
Assistant token: the phone only ever holds a credential that can do what this
integration exposes, never an admin token for the whole house.
"""

from __future__ import annotations

import asyncio
import base64
import hmac
import logging
from typing import TYPE_CHECKING, Any

from aiohttp import web

from homeassistant.components.http import HomeAssistantView
from homeassistant.core import Context, Event, HomeAssistant, callback
from homeassistant.exceptions import HomeAssistantError, ServiceValidationError

from . import imaging
from .const import (
    API_BASE,
    CONF_ACTIONS,
    CONF_AGENT,
    CONF_ALARM,
    CONF_ALARM_CODE,
    CONF_KEY,
    CONF_LOCKS,
    CONF_MEDIA,
    CONF_PIN,
    DOMAIN,
    MAX_WAIT,
)

if TYPE_CHECKING:
    from .hub import PebbleHub

_LOGGER = logging.getLogger(__name__)


def _hub(hass: HomeAssistant, request: web.Request) -> PebbleHub | None:
    key = request.headers.get("X-Pebble-Key", "")
    for entry in hass.config_entries.async_loaded_entries(DOMAIN):
        expected = entry.data.get(CONF_KEY, "")
        if expected and hmac.compare_digest(key.encode(), expected.encode()):
            return entry.runtime_data
    return None


class _Base(HomeAssistantView):
    requires_auth = False
    cors_allowed = True

    def _auth(self, request: web.Request) -> PebbleHub:
        hub = _hub(request.app["hass"], request)
        if hub is None:
            raise web.HTTPUnauthorized(text="bad key")
        return hub


class SyncView(_Base):
    """Long-poll: returns the state plus new events when anything changes."""

    url = f"{API_BASE}/sync"
    name = f"api:{DOMAIN}:sync"

    async def get(self, request: web.Request) -> web.Response:
        hub = self._auth(request)
        hub.touch()
        try:
            since_v = int(request.query.get("v", "-1"))
            since_e = int(request.query.get("e", "0"))
            wait = min(float(request.query.get("wait", "0")), MAX_WAIT)
        except ValueError:
            raise web.HTTPBadRequest from None
        if wait > 0:
            await hub.wait_for_change(since_v, wait)
        state = hub.snapshot()
        # First contact: give the newest event so the watch has history,
        # without replaying 30 old buzzes.
        events = hub.events_since(since_e) if since_e else []
        return self.json({"state": state, "events": events[-5:]})


class EventsView(_Base):
    """Recent event history, newest first."""

    url = f"{API_BASE}/events"
    name = f"api:{DOMAIN}:events"

    async def get(self, request: web.Request) -> web.Response:
        hub = self._auth(request)
        evs = [e.as_wire() for e in reversed(hub.events)]
        return self.json({"events": evs[: int(request.query.get("n", "15"))]})


class ImageView(_Base):
    """A camera (live) or event snapshot, converted for the watch."""

    url = f"{API_BASE}/image"
    name = f"api:{DOMAIN}:image"

    async def get(self, request: web.Request) -> web.Response:
        hub = self._auth(request)
        q = request.query
        src: bytes | None = None
        if "event" in q:
            try:
                src = hub.images.get(int(q["event"]))
            except ValueError:
                raise web.HTTPBadRequest from None
        elif "camera" in q:
            cam = q["camera"]
            if cam not in hub.cameras:
                raise web.HTTPForbidden(text="camera not exposed")
            src = await hub.async_camera_jpeg(hub.resolve_camera(cam))
        elif "media" in q:
            player = q["media"]
            if player not in hub._list(CONF_MEDIA):
                raise web.HTTPForbidden(text="player not exposed")
            src = await hub.async_media_art(player)
        if not src:
            raise web.HTTPNotFound(text="no image")
        try:
            blob = await request.app["hass"].async_add_executor_job(
                imaging.convert,
                src,
                int(q.get("w", 260)),
                int(q.get("h", 170)),
                int(q.get("bpp", 4)),
                q.get("fit", "auto"),
            )
        except (ValueError, OSError) as err:
            _LOGGER.warning("Image conversion failed: %s", err)
            raise web.HTTPUnprocessableEntity from err
        if q.get("b64"):
            return web.Response(text=base64.b64encode(blob).decode(), content_type="text/plain")
        return web.Response(body=blob, content_type="application/octet-stream")


class HelloView(_Base):
    """The watch reports battery and model; returns the state."""

    url = f"{API_BASE}/hello"
    name = f"api:{DOMAIN}:hello"

    async def post(self, request: web.Request) -> web.Response:
        hub = self._auth(request)
        try:
            data = await request.json()
        except ValueError:
            data = {}
        hub.update_watch(data if isinstance(data, dict) else {})
        return self.json({"state": hub.snapshot(), "events": hub.events_since(0)[-1:]})


class ActionView(_Base):
    """Do something: alarm, lock, run a quick action, ask Assist."""

    url = f"{API_BASE}/action"
    name = f"api:{DOMAIN}:action"

    async def post(self, request: web.Request) -> web.Response:
        hub = self._auth(request)
        hass: HomeAssistant = request.app["hass"]
        try:
            body: dict[str, Any] = await request.json()
        except ValueError:
            raise web.HTTPBadRequest from None
        act = body.get("a")
        ctx = Context()
        try:
            if act == "alarm":
                return await self._alarm(hass, hub, body, ctx)
            if act == "lock":
                return await self._lock(hass, hub, body, ctx)
            if act == "run":
                return await self._run(hass, hub, body, ctx)
            if act == "media":
                return await self._media(hass, hub, body, ctx)
            if act == "assist":
                return await self._assist(hass, hub, body, ctx)
            if act == "notify":
                return await self._notify_action(hass, hub, body, ctx)
        except (HomeAssistantError, ServiceValidationError) as err:
            return self.json({"ok": False, "err": str(err)[:60]})
        raise web.HTTPBadRequest(text="unknown action")

    async def _check_pin(self, hub, body) -> web.Response | None:
        """Gate dangerous actions on the watch PIN (if set), checked here, not on the panel.

        Visonic in Powerlink mode (and many other panels) will disarm with any
        code or none at all, so the panel cannot be the check.
        """
        pin = str(hub.opts.get(CONF_PIN) or "")
        if not pin:
            return None  # no PIN configured in HA: the watch app may guard locally
        left = hub.pin_locked()
        if left == float("inf"):
            return self.json({"ok": False, "err": "PIN locked. Reset in HA"})
        if left > 0:
            return self.json({"ok": False, "err": f"Locked {int(left) + 1}s"})
        ok = hmac.compare_digest(str(body.get("code", "")).encode(), pin.encode())
        await hub.async_pin_result(ok)
        if not ok:
            return self.json({"ok": False, "err": "Wrong PIN"})
        return None

    async def _alarm(self, hass, hub, body, ctx) -> web.Response:
        alarm = hub.opts.get(CONF_ALARM)
        mode = body.get("m")
        services = {
            "arm_away": "alarm_arm_away",
            "arm_home": "alarm_arm_home",
            "arm_night": "alarm_arm_night",
            "disarm": "alarm_disarm",
        }
        if not alarm or mode not in services:
            raise web.HTTPBadRequest
        current = hass.states.get(alarm)
        # Cancelling the exit delay isn't disarming a protected house, so it
        # needs no PIN. Disarming once armed (or during entry delay) does.
        cancelling = mode == "disarm" and current is not None and current.state == "arming"
        if mode == "disarm" and not cancelling and (denied := await self._check_pin(hub, body)):
            return denied
        data: dict[str, Any] = {"entity_id": alarm}
        if hub.opts.get(CONF_ALARM_CODE):
            data["code"] = hub.opts[CONF_ALARM_CODE]
        await hass.services.async_call(
            "alarm_control_panel", services[mode], data, blocking=True, context=ctx
        )
        return self.json({"ok": True})

    async def _notify_action(self, hass, hub, body, ctx) -> web.Response:
        """A notification button pressed on the watch.

        Fires the same ``mobile_app_notification_action`` event the phone does,
        so the automations that already answer the phone's buttons answer the
        watch too, and ``pebble_home_notification_action`` for watch-only ones.
        """
        try:
            ev = hub.event_by_ref(int(body.get("id")))
            idx = int(body.get("m"))
        except (TypeError, ValueError):
            raise web.HTTPBadRequest from None
        if ev is None or not 0 <= idx < len(ev.actions):
            return self.json({"ok": False, "err": "Notification expired"})
        button = ev.actions[idx]
        if (button.get("destructive") or button.get("auth")) and (denied := await self._check_pin(hub, body)):
            return denied
        data = {"action": button["action"], "tag": ev.tag, "source": DOMAIN, "event_id": ev.id}
        outcome = _LockOutcome(hass, ctx)
        try:
            hass.bus.async_fire("mobile_app_notification_action", data, context=ctx)
            hass.bus.async_fire(f"{DOMAIN}_notification_action", data, context=ctx)
            result = await outcome.wait()
        finally:
            outcome.close()
        if result is None:
            return self.json({"ok": True, "speech": f"{button['title']}: sent"})
        name, wanted, state = result
        if state in wanted:
            return self.json({"ok": True, "speech": f"{name} {state}"})
        verb = "lock" if "locked" in wanted else "unlock"
        return self.json({"ok": False, "err": f"{name} did not {verb}"[:60]})

    async def _lock(self, hass, hub, body, ctx) -> web.Response:
        lock_id = body.get("id")
        mode = body.get("m")
        if lock_id not in hub._list(CONF_LOCKS) or mode not in ("lock", "unlock", "open"):
            raise web.HTTPForbidden
        if mode in ("unlock", "open") and (denied := await self._check_pin(hub, body)):
            return denied
        await hass.services.async_call("lock", mode, {"entity_id": lock_id}, blocking=True, context=ctx)
        return self.json({"ok": True})

    async def _media(self, hass, hub, body, ctx) -> web.Response:
        player = body.get("id")
        mode = body.get("m")
        if player not in hub._list(CONF_MEDIA):
            raise web.HTTPForbidden
        simple = {
            "play_pause": "media_play_pause",
            "next": "media_next_track",
            "prev": "media_previous_track",
            "stop": "media_stop",
        }
        data: dict[str, Any] = {"entity_id": player}
        if mode in simple:
            service = simple[mode]
        elif mode in ("vol_up", "vol_down", "vol_set"):
            st = hass.states.get(player)
            cur = (st.attributes.get("volume_level") if st else None) or 0.0
            if mode == "vol_set":
                target = int(body.get("code") or 0) / 100
            else:
                # Fixed 4% steps feel right on a button and match Sonos' own.
                target = cur + (0.04 if mode == "vol_up" else -0.04)
            service = "volume_set"
            data["volume_level"] = round(max(0.0, min(1.0, target)), 2)
        elif mode == "mute":
            st = hass.states.get(player)
            service = "volume_mute"
            data["is_volume_muted"] = not bool(st and st.attributes.get("is_volume_muted"))
        else:
            raise web.HTTPBadRequest
        await hass.services.async_call("media_player", service, data, blocking=True, context=ctx)
        return self.json({"ok": True, "quiet": True})

    async def _run(self, hass, hub, body, ctx) -> web.Response:
        target = body.get("id")
        if target not in hub._list(CONF_ACTIONS):
            raise web.HTTPForbidden
        domain = target.split(".")[0]
        service = {
            "script": "turn_on",
            "scene": "turn_on",
            "button": "press",
            "input_button": "press",
            "automation": "trigger",
        }.get(domain, "toggle")
        await hass.services.async_call(domain, service, {"entity_id": target}, blocking=True, context=ctx)
        return self.json({"ok": True})

    async def _assist(self, hass, hub, body, ctx) -> web.Response:
        text = str(body.get("text", "")).strip()[:300]
        if not text:
            raise web.HTTPBadRequest
        data: dict[str, Any] = {"text": text}
        if agent := hub.opts.get(CONF_AGENT):
            data["agent_id"] = agent
        if cid := body.get("cid"):
            data["conversation_id"] = str(cid)[:64]
        resp = await hass.services.async_call(
            "conversation", "process", data, blocking=True, context=ctx, return_response=True
        )
        speech = (
            (resp or {}).get("response", {}).get("speech", {}).get("plain", {}).get("speech", "")
        )
        return self.json({"ok": True, "speech": speech[:400], "cid": (resp or {}).get("conversation_id")})


VIEWS = (SyncView, EventsView, ImageView, HelloView, ActionView)


LOCK_NOTICE_S = 4  # an automation answering a button touches its lock within this
LOCK_SETTLE_S = 35  # TTLock can take ~20 s to give up over Bluetooth


class _LockOutcome:
    """What a lock did because of a watch button, so the watch never says "Unlocked" when it isn't.

    The button only fires an event; whatever automation answers it runs in a
    child context. The first lock that changes in that context is the one the
    button was for. Its next settled state (whoever reports it) is the answer.
    """

    def __init__(self, hass: HomeAssistant, ctx: Context) -> None:
        self._ctx = ctx
        self._entity: str | None = None
        self._name = ""
        self._wanted: tuple[str, ...] = ("unlocked", "open")
        self._touched = asyncio.Event()
        self._settled: asyncio.Future[str] = hass.loop.create_future()
        self._unsub = hass.bus.async_listen("state_changed", self._changed)

    @callback
    def _changed(self, event: Event) -> None:
        new = event.data.get("new_state")
        if new is None or not new.entity_id.startswith("lock."):
            return
        if self._entity is None:
            if self._ctx.id not in (new.context.id, new.context.parent_id):
                return
            self._entity = new.entity_id
            self._name = str(new.attributes.get("friendly_name") or new.entity_id)
            if new.state in ("locking", "locked"):
                self._wanted = ("locked",)
            self._touched.set()
        if new.entity_id != self._entity or self._settled.done():
            return
        if new.state not in ("unlocking", "locking", "opening", "unavailable", "unknown"):
            self._settled.set_result(new.state)

    async def wait(self) -> tuple[str, tuple[str, ...], str] | None:
        try:
            async with asyncio.timeout(LOCK_NOTICE_S):
                await self._touched.wait()
        except TimeoutError:
            return None  # no lock involved: the button did something else
        try:
            async with asyncio.timeout(LOCK_SETTLE_S):
                return self._name, self._wanted, await self._settled
        except TimeoutError:
            return self._name, self._wanted, "not answering"

    def close(self) -> None:
        self._unsub()
