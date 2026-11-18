"""The Pebble Home hub: watches Home Assistant and keeps a feed for the watch."""

from __future__ import annotations

import asyncio
from collections import deque
from dataclasses import dataclass, field
import json
import logging
import time
from typing import Any

from homeassistant.components import camera
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import ATTR_FRIENDLY_NAME, STATE_ON, STATE_UNAVAILABLE, STATE_UNKNOWN
from homeassistant.core import Event, EventStateChangedData, HomeAssistant, callback
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers.dispatcher import async_dispatcher_send
from homeassistant.helpers.event import async_track_state_change_event
from homeassistant.helpers.storage import Store

from . import sources
from .const import (
    CONF_ACTIONS,
    CONF_ALARM,
    CONF_CAMERAS,
    CONF_DOORBELL,
    CONF_DOORBELL_CAMERA,
    CONF_ENTRY_DELAY,
    CONF_EXIT_DELAY,
    DEFAULT_DELAY,
    CONF_FRIGATE,
    CONF_FRIGATE_LABELS,
    CONF_FRIGATE_VIBE,
    CONF_FRIGATE_ZONES_ONLY,
    CONF_LOCKS,
    CONF_MEDIA,
    CONF_PEOPLE,
    CONF_PERIMETER,
    CONF_PIN,
    CONF_STATUS,
    CONF_WEATHER,
    DEFAULT_FRIGATE_LABELS,
    DOMAIN,
    DOORBELL_DEBOUNCE,
    NOTIFY_JOIN,
    FRIGATE_VIBE_ALWAYS,
    FRIGATE_VIBE_ARMED,
    KIND_ALARM,
    KIND_ALERT,
    KIND_DOORBELL,
    KIND_PACKAGE,
    KIND_PERSON,
    MAX_EVENTS,
    PIN_HARD_LOCK,
    VIBES,
)

_LOGGER = logging.getLogger(__name__)

SIGNAL_WATCH_UPDATE = f"{DOMAIN}_watch_update"

# OctoPrint's entities, found by their unique id prefix ("Job Percentage-<uuid>").
PRINTER_PARTS = {
    "printing": ("binary_sensor", "Printing-"),
    "pct": ("sensor", "Job Percentage-"),
    "finish": ("sensor", "Estimated Finish Time-"),
    "state": ("sensor", "Current State-"),
}


def find_printer(hass: HomeAssistant) -> dict[str, str]:
    """The first OctoPrint printer's entities, or {} when there is none."""
    found: dict[str, dict[str, str]] = {}
    for ent in er.async_get(hass).entities.values():
        if ent.platform != "octoprint" or ent.disabled_by:
            continue
        for part, (domain, prefix) in PRINTER_PARTS.items():
            if ent.domain == domain and ent.unique_id.startswith(prefix):
                found.setdefault(ent.unique_id[len(prefix):], {})[part] = ent.entity_id
    for parts in found.values():
        if "printing" in parts and "pct" in parts:
            return parts
    return {}


ALARM_SHORT = {
    "disarmed": "Disarmed",
    "armed_away": "Armed away",
    "armed_home": "Armed home",
    "armed_night": "Armed night",
    "armed_vacation": "Armed vacation",
    "armed_custom_bypass": "Armed custom",
    "arming": "Arming",
    "pending": "Entry delay",
    "triggered": "TRIGGERED",
    "disarming": "Disarming",
}


@dataclass
class WatchEvent:
    """One entry in the watch's feed."""

    id: int
    ts: int
    kind: str
    title: str
    msg: str
    cam: str | None = None
    vibe: str = "short"
    has_img: bool = False
    tag: str | None = None
    actions: list[dict[str, Any]] = field(default_factory=list)

    def as_wire(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "ts": self.ts,
            "k": self.kind,
            "t": self.title[:40],
            "m": self.msg[:120],
            "c": self.cam,
            "v": self.vibe,
            "i": 1 if self.has_img else 0,
            # Notification buttons: titles, and a bitmask of the ones that need
            # a confirm (or the PIN) on the watch first.
            "a": "|".join(a["title"].replace("|", "/") for a in self.actions),
            "af": sum(1 << i for i, a in enumerate(self.actions) if a.get("destructive") or a.get("auth")),
        }


@dataclass
class WatchInfo:
    """What the watch last told us about itself."""

    battery: int | None = None
    charging: bool = False
    model: str | None = None
    firmware: str | None = None
    app: str | None = None
    last_seen: float | None = None
    extra: dict[str, Any] = field(default_factory=dict)


_NOISE = {"motion", "sensor", "air", "current", "the"}
_ABBREV = {"temperature": "temp", "humidity": "humid", "production": "prod"}


def _short_name(name: str) -> str:
    """'SolarEdge SE6000H Power' -> 'SolarEdge Power'; 'Lounge motion Air temperature' -> 'Lounge temp'.

    Drops model numbers and filler words so a reading fits a watch row.
    """
    words = []
    for w in name.split():
        lw = w.lower()
        if lw in _NOISE:
            continue
        if any(c.isdigit() for c in w) and any(c.isalpha() for c in w):
            continue  # looks like a model number
        words.append(_ABBREV.get(lw, w))
    return " ".join(words) or name


class PebbleHub:
    """Owns the event feed, the state snapshot and the long-poll waiters."""

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry) -> None:
        self.hass = hass
        self.entry = entry
        self.events: deque[WatchEvent] = deque(maxlen=MAX_EVENTS)
        self.images: dict[int, bytes] = {}
        self.watch = WatchInfo()
        self._seq = int(time.time())  # survives restarts without reusing ids
        self.version = 0
        self._cond = asyncio.Condition()
        self._unsubs: list = []
        self._last_doorbell = 0.0
        # The event for the current doorbell press, so a notification sent by
        # the same press joins it instead of buzzing twice.
        self._door_task: asyncio.Future[WatchEvent] | None = None
        self._last_notify: tuple[float, asyncio.Future[WatchEvent], str | None] | None = None
        self._state_cache: dict[str, Any] | None = None
        self._last_digest: str | None = None
        self._frigate_seen: dict[str, float] = {}
        self._store: Store = Store(hass, 1, f"{DOMAIN}.{entry.entry_id}.pin")
        self.pin_failures = 0
        self.pin_locked_until = 0.0
        self.printer: dict[str, str] = {}

    # ------------------------------------------------------------------ config
    @property
    def opts(self) -> dict[str, Any]:
        return {**self.entry.data, **self.entry.options}

    def _list(self, key: str) -> list[str]:
        val = self.opts.get(key) or []
        return [val] if isinstance(val, str) else list(val)

    @property
    def cameras(self) -> list[str]:
        cams = self._list(CONF_CAMERAS)
        door = self.opts.get(CONF_DOORBELL_CAMERA)
        if door and door not in cams:
            cams.insert(0, door)
        return cams

    def _cam_up(self, cid: str) -> bool:
        st = self.hass.states.get(cid)
        return st is not None and st.state not in (STATE_UNAVAILABLE, STATE_UNKNOWN)

    def _cam_short_names(self) -> dict[str, str]:
        """'NVR External Porch' -> 'Porch' when other cameras share 'NVR External'."""
        words = {c: self._name(c).replace("_", " ").split() for c in self.cameras}
        out: dict[str, str] = {}
        for cid, w in words.items():
            name = " ".join(w) or cid
            for k in range(len(w) - 1, 0, -1):
                if sum(1 for o in words.values() if len(o) > k and o[:k] == w[:k]) >= 2:
                    name = " ".join(w[k:])
                    break
            out[cid] = name[:1].upper() + name[1:]
        return out

    def visible_cameras(self) -> list[tuple[str, str]]:
        """Cameras worth offering: online, one per name, in configured order.

        Two sources for the same view (Frigate and an NVR, say) collapse to
        whichever is online, so the list keeps working when one goes down.
        """
        names = self._cam_short_names()
        out: list[tuple[str, str]] = []
        seen: set[str] = set()
        for cid in self.cameras:
            key = names[cid].lower()
            if key in seen or not self._cam_up(cid):
                continue
            seen.add(key)
            out.append((cid, names[cid]))
        return out

    def resolve_camera(self, cid: str | None) -> str | None:
        """An online camera with the same view as ``cid``, or ``cid`` itself."""
        if not cid or self._cam_up(cid):
            return cid
        names = self._cam_short_names()
        want = names.get(cid, self._name(cid)).lower()
        for other, n in names.items():
            if other != cid and n.lower() == want and self._cam_up(other):
                return other
        return cid

    def watched_entities(self) -> list[str]:
        ids: list[str] = []
        if self.opts.get(CONF_ALARM):
            ids.append(self.opts[CONF_ALARM])
        if self.opts.get(CONF_WEATHER):
            ids.append(self.opts[CONF_WEATHER])
        for key in (CONF_PEOPLE, CONF_LOCKS, CONF_PERIMETER, CONF_STATUS, CONF_ACTIONS, CONF_MEDIA):
            ids += self._list(key)
        ids += self.printer.values()
        return ids

    # --------------------------------------------------------------- lifecycle
    async def async_start(self) -> None:
        saved = await self._store.async_load() or {}
        self.pin_failures = int(saved.get("failures", 0))
        self.pin_locked_until = float(saved.get("locked_until", 0))
        self.printer = find_printer(self.hass)
        self._last_digest = self._digest()
        watched = self.watched_entities()
        if watched:
            self._unsubs.append(
                async_track_state_change_event(self.hass, watched, self._on_watched_change)
            )
        doorbells = self._list(CONF_DOORBELL)
        if doorbells:
            self._unsubs.append(
                async_track_state_change_event(self.hass, doorbells, self._on_doorbell)
            )
        if self.opts.get(CONF_FRIGATE) and "mqtt" in self.hass.config.components:
            from homeassistant.components import mqtt

            try:
                self._unsubs.append(
                    await mqtt.async_subscribe(self.hass, "frigate/events", self._on_frigate)
                )
            except Exception:  # noqa: BLE001 - MQTT down must not block setup
                _LOGGER.warning("Could not subscribe to frigate/events; Frigate alerts off")

    async def async_stop(self) -> None:
        for unsub in self._unsubs:
            unsub()
        self._unsubs.clear()
        async with self._cond:
            self._cond.notify_all()

    # ------------------------------------------------------------------ feed
    async def _bump(self) -> None:
        self._state_cache = None
        self.version += 1
        async with self._cond:
            self._cond.notify_all()

    def _digest(self) -> str:
        self._state_cache = None
        snap = dict(self.snapshot())
        self._state_cache = None
        for volatile in ("v", "now"):
            snap.pop(volatile, None)
        return json.dumps(snap, sort_keys=True, default=str)

    async def _bump_if_changed(self) -> None:
        """Wake the watch only when what it shows has changed.

        Person GPS fixes and weather attributes change constantly; each wake
        costs a Bluetooth round trip on the wrist.
        """
        digest = self._digest()
        if digest == self._last_digest:
            return
        self._last_digest = digest
        await self._bump()

    async def wait_for_change(self, since: int, timeout: float) -> None:
        if self.version != since:
            return
        async with self._cond:
            try:
                await asyncio.wait_for(
                    self._cond.wait_for(lambda: self.version != since), timeout
                )
            except TimeoutError:
                pass

    async def async_add_event(
        self,
        kind: str,
        title: str,
        msg: str = "",
        cam: str | None = None,
        vibe: str = "short",
        image: bytes | None = None,
    ) -> WatchEvent:
        self._seq += 1
        ev = WatchEvent(self._seq, int(time.time()), kind, title, msg, cam, vibe)
        if image is None and cam:
            image = await self.async_camera_jpeg(cam)
        if image:
            ev.has_img = True
            self.images[ev.id] = await self.hass.async_add_executor_job(sources.shrink, image)
        self.events.append(ev)
        live = {e.id for e in self.events}
        for stale in [i for i in self.images if i not in live]:
            del self.images[stale]
        _LOGGER.debug("Watch event %s: %s %s", ev.id, title, msg)
        await self._bump()
        return ev

    async def async_media_art(self, entity_id: str) -> bytes | None:
        from homeassistant.components.media_player.const import DATA_COMPONENT

        component = self.hass.data.get(DATA_COMPONENT)
        player = component.get_entity(entity_id) if component else None
        if player is None:
            return None
        try:
            data, _ = await player.async_get_media_image()
        except Exception as err:  # noqa: BLE001
            _LOGGER.debug("Art for %s failed: %s", entity_id, err)
            return None
        return data

    async def async_camera_jpeg(self, entity_id: str) -> bytes | None:
        try:
            img = await camera.async_get_image(self.hass, entity_id, timeout=8)
        except Exception as err:  # noqa: BLE001
            _LOGGER.debug("Snapshot of %s failed: %s", entity_id, err)
            return None
        return img.content

    # ---------------------------------------------------------------- sources
    @callback
    def _on_watched_change(self, event: Event[EventStateChangedData]) -> None:
        entity_id = event.data["entity_id"]
        old, new = event.data["old_state"], event.data["new_state"]
        if new is None:
            return
        if entity_id == self.opts.get(CONF_ALARM) and old and old.state != new.state:
            self.hass.async_create_task(self._alarm_event(old.state, new))
        else:
            self.hass.async_create_task(self._bump_if_changed())

    async def _alarm_event(self, old_state: str, new) -> None:
        state = new.state
        by = new.attributes.get("changed_by") or ""
        title = ALARM_SHORT.get(state, state.replace("_", " ").capitalize())
        vibe = "short"
        cam = None
        if state == "triggered":
            vibe = "alarm"
            cam = self.resolve_camera(self.opts.get(CONF_DOORBELL_CAMERA))
        elif state in ("pending", "arming"):
            vibe = "double"
        elif state in (STATE_UNAVAILABLE, STATE_UNKNOWN) or old_state in (
            STATE_UNAVAILABLE,
            STATE_UNKNOWN,
        ):
            # Connection blips are not news.
            await self._bump()
            return
        msg = f"by {by}" if by and by != "System" else ""
        await self.async_add_event(KIND_ALARM, f"Alarm {title.lower()}", msg, cam, vibe)

    @callback
    def _on_doorbell(self, event: Event[EventStateChangedData]) -> None:
        new, old = event.data["new_state"], event.data["old_state"]
        if new is None or old is None:
            return
        domain = new.entity_id.split(".")[0]
        if domain == "event":
            fired = new.state not in (STATE_UNKNOWN, STATE_UNAVAILABLE) and new.state != old.state
        else:
            fired = new.state == STATE_ON and old.state != STATE_ON
        if not fired:
            return
        now = time.monotonic()
        if now - self._last_doorbell < DOORBELL_DEBOUNCE:
            return
        self._last_doorbell = now
        recent = self._last_notify
        if recent and now - recent[0] < NOTIFY_JOIN and self._is_door_cam(recent[2]):
            # The automation's notification beat us to it: make that the doorbell.
            self._door_task = recent[1]
            self.hass.async_create_task(self._make_doorbell(recent[1]))
            return
        cam = self.resolve_camera(self.opts.get(CONF_DOORBELL_CAMERA))
        self._door_task = self.hass.async_create_task(
            self.async_add_event(KIND_DOORBELL, "Doorbell", "Someone at the door", cam, "doorbell")
        )

    def _is_door_cam(self, cam: str | None) -> bool:
        door = self.opts.get(CONF_DOORBELL_CAMERA)
        if not cam or not door:
            return False
        return cam == door or self.resolve_camera(cam) == self.resolve_camera(door)

    async def _make_doorbell(self, task: asyncio.Future[WatchEvent]) -> None:
        ev = await task
        ev.kind, ev.vibe = KIND_DOORBELL, "doorbell"
        if not ev.cam:
            ev.cam = self._live_cam(self.opts.get(CONF_DOORBELL_CAMERA))
        await self._bump()

    def _live_cam(self, cam: str | None) -> str | None:
        """A camera the watch may stream itself, or None (snapshot only)."""
        cam = self.resolve_camera(cam)
        return cam if cam and cam in self.cameras else None

    # ------------------------------------------------------------ notifications
    async def async_notify(
        self, message: str = "", title: str | None = None, data: dict[str, Any] | None = None
    ) -> WatchEvent | None:
        """A notification for the watch, in the shape mobile_app takes.

        Understands ``image``, ``attachment.url``, ``entity_id`` / ``camera``
        (a camera: snapshot now, live view on the watch), ``tag`` (replaces an
        earlier one), ``message: clear_notification``, ``vibe``, ``kind`` and
        iOS ``push.interruption-level``. Anything else (url, actions, group...)
        is ignored, so a phone notification's data can be pasted unchanged.
        """
        data = dict(data or {})
        tag = data.get("tag")
        tag = str(tag) if tag not in (None, "") else None
        if message == "clear_notification":
            if tag:
                self._drop_tag(tag)
                await self._bump()
            return None

        cam = sources.camera_from(data.get("camera")) or sources.camera_from(data.get("entity_id"))
        image_src = data.get("image")
        if not image_src and isinstance(data.get("attachment"), dict):
            image_src = data["attachment"].get("url")
        if not cam and isinstance(image_src, str) and sources.camera_from(image_src):
            cam, image_src = sources.camera_from(image_src), None

        kind = str(data.get("kind") or KIND_ALERT)[:11]
        vibe = data.get("vibe")
        if vibe not in VIBES:
            level = (data.get("push") or {}).get("interruption-level") if isinstance(data.get("push"), dict) else None
            vibe = {"passive": "none", "critical": "alarm"}.get(level) or (
                "doorbell" if kind == KIND_DOORBELL else "short"
            )
        actions = [
            {"action": str(a.get("action")), "title": str(a.get("title") or a.get("action"))[:20],
             "destructive": bool(a.get("destructive")), "auth": bool(a.get("authenticationRequired"))}
            for a in (data.get("actions") or [])
            if isinstance(a, dict) and a.get("action") and str(a["action"]).upper() not in ("URI", "REPLY")
            and not a.get("uri")
        ][:4]

        now = time.monotonic()
        door_like = kind == KIND_DOORBELL or self._is_door_cam(cam)
        if door_like and self._door_task and now - self._last_doorbell < DOORBELL_DEBOUNCE:
            # Same press as the doorbell event already on its way: join it.
            ev = await self._door_task
            if title:
                ev.title = title
            if message:
                ev.msg = message
            if tag:
                ev.tag = tag
            if actions:
                ev.actions = actions
            if image_src and (img := await sources.async_fetch(self.hass, image_src)):
                self.images[ev.id] = await self.hass.async_add_executor_job(sources.shrink, img)
                ev.has_img = True
            await self._bump()
            return ev

        if kind == KIND_DOORBELL:
            self._last_doorbell = now  # the sensor firing a moment later joins this one
        task = self.hass.async_create_task(
            self._notify_event(kind, title, message, cam, vibe, image_src, tag, actions)
        )
        if kind == KIND_DOORBELL:
            self._door_task = task
        self._last_notify = (now, task, cam)
        return await task

    async def _notify_event(self, kind, title, message, cam, vibe, image_src, tag, actions) -> WatchEvent:
        image = await sources.async_fetch(self.hass, image_src) if isinstance(image_src, str) else None
        if image is None and cam:
            image = await self.async_camera_jpeg(self.resolve_camera(cam))
        if tag:
            self._drop_tag(tag)
        default_title = "Doorbell" if kind == KIND_DOORBELL else "Home"
        ev = await self.async_add_event(
            kind, title or default_title, message or "", self._live_cam(cam), vibe, image or b""
        )
        ev.tag = tag
        ev.actions = actions
        return ev

    def _drop_tag(self, tag: str) -> None:
        for ev in [e for e in self.events if e.tag == tag]:
            self.events.remove(ev)
            self.images.pop(ev.id, None)

    async def _on_frigate(self, msg) -> None:
        try:
            payload = json.loads(msg.payload)
        except (ValueError, TypeError):
            return
        if payload.get("type") not in ("new", "update"):
            return
        after = payload.get("after") or {}
        event_id = after.get("id")
        label = after.get("label")
        labels = self._list(CONF_FRIGATE_LABELS) or DEFAULT_FRIGATE_LABELS
        if not event_id or label not in labels or after.get("false_positive"):
            return
        if event_id in self._frigate_seen:
            return
        # current_zones is usually empty on "new"; objects walk into zones in
        # later "update" messages, so wait for the first one that has a zone.
        zones = after.get("current_zones") or after.get("entered_zones") or []
        if self.opts.get(CONF_FRIGATE_ZONES_ONLY, True) and not zones:
            return
        now = time.monotonic()
        self._frigate_seen[event_id] = now
        for old_id in [k for k, t in self._frigate_seen.items() if now - t > 3600]:
            del self._frigate_seen[old_id]
        cam_name = after.get("camera", "")
        cam_entity = f"camera.{cam_name}"
        if self.cameras and cam_entity not in self.cameras:
            return

        mode = self.opts.get(CONF_FRIGATE_VIBE, FRIGATE_VIBE_ARMED)
        armed = False
        if alarm := self.opts.get(CONF_ALARM):
            st = self.hass.states.get(alarm)
            armed = bool(st and st.state.startswith("armed"))
        vibe = "none"
        if mode == FRIGATE_VIBE_ALWAYS or (mode == FRIGATE_VIBE_ARMED and armed):
            vibe = "double"
        if label == "package":
            vibe = "short" if vibe == "none" else vibe

        where = (zones[0] if zones else cam_name).replace("_", " ")
        kind = KIND_PACKAGE if label == "package" else KIND_PERSON
        title = "Parcel" if label == "package" else label.capitalize()
        image = await self._frigate_snapshot(after.get("id"))
        await self.async_add_event(
            kind, title, f"{where.title()}", cam_entity, vibe, image=image
        )

    async def _frigate_snapshot(self, event_id: str | None) -> bytes | None:
        """Fetch the event snapshot (with the box drawn) straight from Frigate."""
        if not event_id:
            return None
        entries = self.hass.config_entries.async_entries("frigate")
        if not entries:
            return None
        base = (entries[0].data.get("url") or "").rstrip("/")
        if not base:
            return None
        session = async_get_clientsession(self.hass)
        url = f"{base}/api/events/{event_id}/snapshot.jpg?bbox=1&quality=85"
        for _ in range(3):
            await asyncio.sleep(1.2)  # snapshot is written just after "new"
            try:
                async with asyncio.timeout(6):
                    resp = await session.get(url)
                    if resp.status == 200:
                        return await resp.read()
            except (TimeoutError, OSError) as err:
                _LOGGER.debug("Frigate snapshot %s: %s", event_id, err)
        return None

    # ------------------------------------------------------------------ state
    def _name(self, entity_id: str) -> str:
        st = self.hass.states.get(entity_id)
        if st is None:
            return entity_id.split(".", 1)[-1].replace("_", " ").title()
        return str(st.attributes.get(ATTR_FRIENDLY_NAME, entity_id))

    def _print_job(self) -> dict[str, Any] | None:
        """Progress of the print under way (or paused), else None."""
        if not self.printer:
            return None
        get = self.hass.states.get
        printing = get(self.printer["printing"])
        state = get(self.printer.get("state", ""))
        paused = bool(state and state.state.lower().startswith("paus"))
        if not paused and not (printing and printing.state == STATE_ON):
            return None
        try:
            pct = max(0, min(100, round(float(get(self.printer["pct"]).state))))
        except (AttributeError, TypeError, ValueError):
            pct = 0
        finish = 0
        if (st := get(self.printer.get("finish", ""))) and st.state not in (STATE_UNAVAILABLE, STATE_UNKNOWN):
            try:
                from homeassistant.util import dt as dt_util

                when = dt_util.parse_datetime(st.state)
                # Whole minutes: the estimate drifts by seconds on every poll,
                # and each change would wake the watch.
                finish = int(when.timestamp()) // 60 * 60 if when else 0
            except (TypeError, ValueError):
                finish = 0
        return {"p": pct, "e": finish, "s": "paused" if paused else "printing"}

    def snapshot(self) -> dict[str, Any]:
        """Compact state for the watch. Keys are short: AppMessage is small."""
        if self._state_cache is not None:
            return self._state_cache
        hass = self.hass
        out: dict[str, Any] = {"v": self.version, "now": int(time.time())}

        if alarm := self.opts.get(CONF_ALARM):
            st = hass.states.get(alarm)
            if st:
                out["alarm"] = {
                    "s": st.state,
                    "l": ALARM_SHORT.get(st.state, st.state),
                    "r": bool(st.attributes.get("ready", True)),
                    "by": st.attributes.get("changed_by") or "",
                    "t": int(st.last_changed.timestamp()),
                    # Which arm modes the panel offers (HA feature bits:
                    # 1 home, 2 away, 4 night, 32 vacation).
                    "f": int(st.attributes.get("supported_features") or 3),
                }
                # Panels report the exit/entry delay as a state but not how
                # long is left, so count down from when the state began.
                if st.state in ("arming", "pending"):
                    delay = int(self.opts.get(
                        CONF_EXIT_DELAY if st.state == "arming" else CONF_ENTRY_DELAY, DEFAULT_DELAY))
                    out["alarm"]["until"] = int(st.last_changed.timestamp()) + delay
                    # Visonic says where it is heading (arming_away, arming_home).
                    panel = str(st.attributes.get("state") or "")
                    out["alarm"]["to"] = (
                        "home" if "home" in panel else "night" if "night" in panel
                        else "away" if st.state == "arming" else ""
                    )
                    if out["alarm"]["to"] and st.state == "arming":
                        out["alarm"]["l"] = f"Arming {out['alarm']['to']}"

        people = []
        for pid in self._list(CONF_PEOPLE):
            st = hass.states.get(pid)
            if st:
                # Sub-zones inside home (a "porch" zone, say) still count as home.
                home = st.state == "home" or "zone.home" in (st.attributes.get("in_zones") or [])
                people.append({"n": self._name(pid).split(" ")[0], "h": home})
        out["ppl"] = people

        locks = []
        for lid in self._list(CONF_LOCKS):
            st = hass.states.get(lid)
            if st:
                locks.append({"id": lid, "n": self._name(lid), "s": st.state})
        out["locks"] = locks

        open_ = [self._name(e) for e in self._list(CONF_PERIMETER)
                 if (st := hass.states.get(e)) and st.state == STATE_ON]
        out["open"] = open_[:6]
        out["open_n"] = len(open_)

        if weather := self.opts.get(CONF_WEATHER):
            st = hass.states.get(weather)
            if st and st.state not in (STATE_UNAVAILABLE, STATE_UNKNOWN):
                temp = st.attributes.get("temperature")
                out["wx"] = {
                    "c": st.state,
                    "t": round(float(temp)) if temp is not None else None,
                    "u": st.attributes.get("temperature_unit", ""),
                }

        status = []
        for sid in self._list(CONF_STATUS):
            st = hass.states.get(sid)
            if not st:
                continue
            val = st.state
            if val in (STATE_UNAVAILABLE, STATE_UNKNOWN):
                status.append({"n": _short_name(self._name(sid))[:18], "s": "-"})
                continue
            try:
                f = float(val)
                val = f"{f:.0f}" if abs(f) >= 10 else f"{f:.1f}".rstrip("0").rstrip(".")
            except ValueError:
                val = val.replace("_", " ")
            status.append({
                "n": _short_name(self._name(sid))[:18],
                "s": f"{val}{st.attributes.get('unit_of_measurement', '') or ''}"[:12],
            })
        out["st"] = status[:6]

        media = []
        for mid in self._list(CONF_MEDIA):
            st = hass.states.get(mid)
            if not st or st.state == STATE_UNAVAILABLE:
                continue
            a = st.attributes
            vol = a.get("volume_level")
            group = a.get("group_members") or []
            media.append({
                "id": mid,
                "n": self._name(mid)[:14],
                "s": st.state,
                "t": str(a.get("media_title") or "")[:40],
                "a": str(a.get("media_artist") or a.get("media_album_name") or "")[:30],
                "vol": round(vol * 100) if isinstance(vol, (int, float)) else -1,
                "mute": 1 if a.get("is_volume_muted") else 0,
                # Followers in a Sonos group mirror the coordinator; show it once.
                "lead": 0 if (len(group) > 1 and group[0] != mid) else 1,
                "grp": len(group),
                # The art changes with the track; the hash lets the watch skip re-fetching.
                "art": (a.get("entity_picture") or "")[-8:],
            })
        out["media"] = media[:14]

        if pr := self._print_job():
            out["pr"] = pr

        out["cams"] = [{"id": c, "n": n[:16]} for c, n in self.visible_cameras()]
        out["acts"] = [{"id": a, "n": self._name(a)[:28]} for a in self._list(CONF_ACTIONS)]
        out["pin_lock"] = 1 if self.pin_locked() else 0
        out["pin"] = 1 if self.opts.get(CONF_PIN) else 0
        last = self.events[-1] if self.events else None
        out["last"] = last.as_wire() if last else None
        self._state_cache = out
        return out

    def events_since(self, since_id: int) -> list[dict[str, Any]]:
        return [e.as_wire() for e in self.events if e.id > since_id]

    def event(self, event_id: int) -> WatchEvent | None:
        return next((e for e in self.events if e.id == event_id), None)

    def event_by_ref(self, ref: int) -> WatchEvent | None:
        """By id, or by its low 28 bits (all a timeline launch code has room for)."""
        return self.event(ref) or next(
            (e for e in reversed(self.events) if e.id & 0x0FFFFFFF == ref), None
        )

    # -------------------------------------------------------------------- pin
    def pin_locked(self) -> float:
        """Seconds left on the lockout (inf for a hard lock), 0 if open."""
        if self.pin_failures >= PIN_HARD_LOCK:
            return float("inf")
        return max(0.0, self.pin_locked_until - time.time())

    async def async_pin_result(self, ok: bool) -> None:
        """Record a PIN attempt. Lockouts escalate and survive restarts.

        3 wrong in a row locks for 30 s, then 60 s, 2 min, 4 min ... and 10 in
        a row locks code actions until someone presses Reset in Home Assistant.
        """
        if ok:
            self.pin_failures, self.pin_locked_until = 0, 0.0
        else:
            self.pin_failures += 1
            n = self.pin_failures
            if n >= 3:
                self.pin_locked_until = time.time() + 30 * 2 ** (n - 3)
            if n in (3, PIN_HARD_LOCK):
                hard = n >= PIN_HARD_LOCK
                await self.hass.services.async_call(
                    "persistent_notification",
                    "create",
                    {
                        "title": "Pebble Home",
                        "notification_id": f"{DOMAIN}_pin",
                        "message": (
                            "10 wrong PINs from the watch. Disarm and unlock from the watch are "
                            "locked until you press Reset PIN lockout on the Pebble device."
                            if hard
                            else "3 wrong PINs from the watch in a row. Lockouts now escalate."
                        ),
                    },
                )
        await self._store.async_save(
            {"failures": self.pin_failures, "locked_until": self.pin_locked_until}
        )
        await self._bump()

    async def async_pin_reset(self) -> None:
        await self.async_pin_result(True)

    # ------------------------------------------------------------------ watch
    def update_watch(self, data: dict[str, Any]) -> None:
        w = self.watch
        if "bat" in data:
            try:
                w.battery = int(data["bat"])
            except (TypeError, ValueError):
                pass
        w.charging = bool(data.get("chg", w.charging))
        w.model = data.get("model", w.model)
        w.firmware = data.get("fw", w.firmware)
        w.app = data.get("app", w.app)
        w.last_seen = time.time()
        async_dispatcher_send(self.hass, SIGNAL_WATCH_UPDATE)

    def touch(self) -> None:
        """Note that the watch is still polling."""
        first = self.watch.last_seen is None
        prev = self.watch.last_seen or 0
        self.watch.last_seen = time.time()
        if first or self.watch.last_seen - prev > 60:
            async_dispatcher_send(self.hass, SIGNAL_WATCH_UPDATE)
