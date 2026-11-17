"""End-to-end tests for Pebble Home against a real Home Assistant core."""

from __future__ import annotations

import asyncio
import io
import json
import struct
from types import SimpleNamespace
from unittest.mock import patch

from PIL import Image
import pytest

from homeassistant.components.camera import Image as CamImage
from homeassistant.core import HomeAssistant
from homeassistant.setup import async_setup_component
from pytest_homeassistant_custom_component.common import MockConfigEntry

from custom_components.pebble_home.const import DOMAIN

KEY = "test-key-123"
H = {"X-Pebble-Key": KEY}


def _jpeg(w=640, h=360, colour=(200, 40, 40)) -> bytes:
    buf = io.BytesIO()
    Image.new("RGB", (w, h), colour).save(buf, "JPEG")
    return buf.getvalue()


@pytest.fixture
async def setup(hass: HomeAssistant, hass_client_no_auth):
    assert await async_setup_component(hass, "http", {})
    assert await async_setup_component(
        hass,
        "alarm_control_panel",
        {"alarm_control_panel": [{"platform": "manual", "name": "House", "code_arm_required": False, "arming_time": 0, "delay_time": 0}]},
    )
    hass.states.async_set("person.chris", "porch", {"friendly_name": "Chris Smith", "in_zones": ["zone.porch", "zone.home"]})
    hass.states.async_set("person.sam", "not_home", {"friendly_name": "Sam"})
    hass.states.async_set("binary_sensor.back_door", "off", {"friendly_name": "Back door", "device_class": "door"})
    hass.states.async_set("binary_sensor.doorbell", "off", {"friendly_name": "Doorbell"})
    hass.states.async_set("weather.home", "rainy", {"temperature": 5.4, "temperature_unit": "°C"})
    hass.states.async_set("camera.porch", "streaming", {"friendly_name": "Porch"})
    hass.states.async_set(
        "media_player.kitchen",
        "playing",
        {"friendly_name": "Kitchen", "media_title": "Penny Lane", "media_artist": "The Beatles",
         "volume_level": 0.2, "group_members": ["media_player.kitchen"], "entity_picture": "/api/x?cache=abc12345"},
    )
    await hass.async_block_till_done()
    entry = MockConfigEntry(
        domain=DOMAIN,
        data={"api_key": KEY},
        options={
            "alarm_entity": "alarm_control_panel.house",
            "people": ["person.chris", "person.sam"],
            "perimeter": ["binary_sensor.back_door"],
            "doorbell_entities": ["binary_sensor.doorbell"],
            "doorbell_camera": "camera.porch",
            "cameras": ["camera.porch"],
            "weather": "weather.home",
            "watch_pin": "4321",
            "media_players": ["media_player.kitchen"],
        },
    )
    entry.add_to_hass(hass)
    with patch("homeassistant.components.camera.async_get_image", return_value=CamImage("image/jpeg", _jpeg())):
        assert await hass.config_entries.async_setup(entry.entry_id)
        await hass.async_block_till_done()
        client = await hass_client_no_auth()
        yield SimpleNamespace(entry=entry, client=client, hub=entry.runtime_data)


async def test_auth_required(hass, setup):
    r = await setup.client.get("/api/pebble_home/v1/sync")
    assert r.status == 401
    r = await setup.client.get(f"/api/pebble_home/v1/sync?key={KEY}")
    assert r.status == 401, "key in the URL must not be accepted"
    r = await setup.client.get("/api/pebble_home/v1/sync", headers=H)
    assert r.status == 200


async def test_state_shape(hass, setup):
    r = await setup.client.get("/api/pebble_home/v1/sync", headers=H)
    s = (await r.json())["state"]
    assert s["alarm"]["s"] == "disarmed"
    assert s["ppl"] == [{"n": "Chris", "h": True}, {"n": "Sam", "h": False}]
    assert s["wx"] == {"c": "rainy", "t": 5, "u": "°C"}
    assert s["open_n"] == 0
    assert s["cams"] == [{"id": "camera.porch", "n": "Porch"}]


async def test_long_poll_wakes_on_real_change_only(hass, setup):
    r = await setup.client.get("/api/pebble_home/v1/sync", headers=H)
    v = (await r.json())["state"]["v"]

    async def poll():
        resp = await setup.client.get(f"/api/pebble_home/v1/sync?wait=3&v={v}", headers=H)
        return await resp.json()

    task = asyncio.create_task(poll())
    await asyncio.sleep(0.2)
    # A GPS-only attribute change must not wake the watch.
    hass.states.async_set("person.chris", "porch", {"friendly_name": "Chris Smith", "in_zones": ["zone.porch", "zone.home"], "latitude": 51.5})
    await hass.async_block_till_done()
    await asyncio.sleep(0.3)
    assert not task.done()
    hass.states.async_set("binary_sensor.back_door", "on", {"friendly_name": "Back door", "device_class": "door"})
    await hass.async_block_till_done()
    res = await asyncio.wait_for(task, 3)
    assert res["state"]["v"] > v
    assert res["state"]["open"] == ["Back door"]


async def test_doorbell_event_with_image(hass, setup):
    hass.states.async_set("binary_sensor.doorbell", "on")
    await hass.async_block_till_done()
    await asyncio.sleep(0.05)
    assert setup.hub.events, "doorbell should create an event"
    ev = setup.hub.events[-1]
    assert ev.kind == "doorbell" and ev.vibe == "doorbell" and ev.has_img
    # Debounced: a second press straight after is ignored.
    hass.states.async_set("binary_sensor.doorbell", "off")
    hass.states.async_set("binary_sensor.doorbell", "on")
    await hass.async_block_till_done()
    assert len(setup.hub.events) == 1

    r = await setup.client.get(f"/api/pebble_home/v1/image?event={ev.id}&w=100&h=60&bpp=4", headers=H)
    blob = await r.read()
    magic, ver, bpp, w, h, stride, n = struct.unpack_from("<2sBBHHHB", blob)
    assert (magic, bpp, w, h, stride, n) == (b"PB", 4, 100, 60, 50, 16)
    assert len(blob) == 11 + n + stride * h


async def test_camera_allowlist(hass, setup):
    r = await setup.client.get("/api/pebble_home/v1/image?camera=camera.secret", headers=H)
    assert r.status == 403


async def test_arm_without_pin_disarm_needs_pin(hass, setup):
    post = lambda body: setup.client.post("/api/pebble_home/v1/action", headers=H, json=body)  # noqa: E731
    r = await post({"a": "alarm", "m": "arm_away"})
    assert (await r.json())["ok"] is True
    await hass.async_block_till_done()
    assert hass.states.get("alarm_control_panel.house").state == "armed_away"
    assert any(e.kind == "alarm" for e in setup.hub.events)

    r = await post({"a": "alarm", "m": "disarm", "code": "0000"})
    assert (await r.json()) == {"ok": False, "err": "Wrong PIN"}
    assert hass.states.get("alarm_control_panel.house").state == "armed_away"

    r = await post({"a": "alarm", "m": "disarm", "code": "4321"})
    assert (await r.json())["ok"] is True
    await hass.async_block_till_done()
    assert hass.states.get("alarm_control_panel.house").state == "disarmed"


async def test_pin_lockout_escalates_and_hard_locks(hass, setup):
    hub = setup.hub
    post = lambda body: setup.client.post("/api/pebble_home/v1/action", headers=H, json=body)  # noqa: E731
    for _ in range(3):
        await post({"a": "alarm", "m": "disarm", "code": "1111"})
    r = await post({"a": "alarm", "m": "disarm", "code": "4321"})
    assert (await r.json())["err"].startswith("Locked"), "right PIN refused during lockout"
    # Fast-forward through lockouts to the hard lock.
    for _ in range(7):
        hub.pin_locked_until = 0
        await post({"a": "alarm", "m": "disarm", "code": "1111"})
    hub.pin_locked_until = 0
    r = await post({"a": "alarm", "m": "disarm", "code": "4321"})
    assert (await r.json())["err"] == "PIN locked. Reset in HA"
    await hass.services.async_call("button", "press", {"entity_id": "button.pebble_reset_pin_lockout"}, blocking=True)
    r = await post({"a": "alarm", "m": "disarm", "code": "4321"})
    assert (await r.json())["ok"] is True


async def test_frigate_fires_once_when_zone_entered(hass, setup):
    hub = setup.hub
    hub.entry.options  # noqa: B018
    base = {"id": "ev1", "label": "person", "camera": "porch", "false_positive": False}

    def msg(kind, zones):
        return SimpleNamespace(payload=json.dumps({"type": kind, "after": {**base, "current_zones": zones}}))

    with patch.object(hub, "_frigate_snapshot", return_value=_jpeg()):
        await hub._on_frigate(msg("new", []))
        assert not hub.events, "no zone yet"
        await hub._on_frigate(msg("update", ["porch_step"]))
        await hub._on_frigate(msg("update", ["porch_step"]))
    evs = [e for e in hub.events if e.kind == "person"]
    assert len(evs) == 1 and evs[0].msg == "Porch Step" and evs[0].vibe == "none"


async def test_alert_service_and_notify(hass, setup):
    res = await hass.services.async_call(
        DOMAIN, "alert", {"title": "Bins", "message": "Green bin"}, blocking=True, return_response=True
    )
    assert res["id"] == setup.hub.events[-1].id
    await hass.services.async_call("notify", "send_message", {"entity_id": "notify.pebble_watch", "message": "hi"}, blocking=True)
    assert setup.hub.events[-1].msg == "hi"


async def test_hello_updates_battery(hass, setup):
    r = await setup.client.post("/api/pebble_home/v1/hello", headers=H, json={"bat": 80, "chg": 1, "app": "face"})
    assert r.status == 200
    await hass.async_block_till_done()
    assert hass.states.get("sensor.pebble_battery").state == "80"


async def test_media_state_and_controls(hass, setup):
    calls = []

    async def record(call):
        calls.append((call.service, dict(call.data)))

    for svc in ("media_play_pause", "media_next_track", "volume_set"):
        hass.services.async_register("media_player", svc, record)
    r = await setup.client.get("/api/pebble_home/v1/sync", headers=H)
    m = (await r.json())["state"]["media"][0]
    assert m == {"id": "media_player.kitchen", "n": "Kitchen", "s": "playing", "t": "Penny Lane",
                 "a": "The Beatles", "vol": 20, "mute": 0, "lead": 1, "grp": 1, "art": "abc12345"}
    post = lambda body: setup.client.post("/api/pebble_home/v1/action", headers=H, json=body)  # noqa: E731
    assert (await (await post({"a": "media", "id": "media_player.kitchen", "m": "play_pause"})).json())["ok"]
    await post({"a": "media", "id": "media_player.kitchen", "m": "vol_up"})
    assert calls[-1] == ("volume_set", {"entity_id": "media_player.kitchen", "volume_level": 0.24})
    r = await post({"a": "media", "id": "media_player.garden", "m": "play_pause"})
    assert r.status == 403, "only configured players"


async def test_no_pin_configured_allows_disarm(hass, setup):
    hass.config_entries.async_update_entry(setup.entry, options={**setup.entry.options, "watch_pin": ""})
    await hass.async_block_till_done()
    client = setup.client
    hub = setup.entry.runtime_data
    await hass.services.async_call("alarm_control_panel", "alarm_arm_away", {"entity_id": "alarm_control_panel.house"}, blocking=True)
    r = await client.get("/api/pebble_home/v1/sync", headers=H)
    assert (await r.json())["state"]["pin"] == 0
    r = await client.post("/api/pebble_home/v1/action", headers=H, json={"a": "alarm", "m": "disarm"})
    assert (await r.json())["ok"] is True
    assert hub is not None


def test_short_names():
    from custom_components.pebble_home.hub import _short_name

    assert _short_name("SolarEdge SE6000H Power") == "SolarEdge Power"
    assert _short_name("Lounge motion Air temperature") == "Lounge temp"


async def test_arming_countdown_and_cancel_without_pin(hass, setup):
    hass.states.async_set("alarm_control_panel.house", "arming", {"state": "arming_away"})
    await hass.async_block_till_done()
    r = await setup.client.get("/api/pebble_home/v1/sync?v=-1", headers=H)
    a = (await r.json())["state"]["alarm"]
    assert a["to"] == "away" and a["l"] == "Arming away"
    assert 25 <= a["until"] - a["t"] <= 30
    # PIN is 4321 in this fixture, but cancelling an exit delay needs none.
    calls = []
    hass.services.async_register("alarm_control_panel", "alarm_disarm", lambda c: calls.append(c.data), )
    r = await setup.client.post("/api/pebble_home/v1/action", headers=H, json={"a": "alarm", "m": "disarm"})
    assert (await r.json())["ok"] is True
    # Entry delay is different: the house is armed, so the PIN applies.
    hass.states.async_set("alarm_control_panel.house", "pending", {})
    await hass.async_block_till_done()
    r = await setup.client.post("/api/pebble_home/v1/action", headers=H, json={"a": "alarm", "m": "disarm"})
    assert (await r.json())["err"] == "Wrong PIN"


async def test_camera_fallback_to_second_source(hass, setup):
    """Frigate down, NVR up: the watch sees one 'Porch' and it works."""
    hass.states.async_set("camera.nvr_porch", "recording", {"friendly_name": "nvr porch"})
    hass.states.async_set("camera.nvr_garden", "recording", {"friendly_name": "nvr_garden"})
    hass.config_entries.async_update_entry(
        setup.entry,
        options={**setup.entry.options, "cameras": ["camera.porch", "camera.nvr_porch", "camera.nvr_garden"]},
    )
    await hass.async_block_till_done()
    hub = setup.entry.runtime_data
    assert hub.visible_cameras() == [("camera.porch", "Porch"), ("camera.nvr_garden", "Garden")]

    hass.states.async_set("camera.porch", "unavailable", {"friendly_name": "Porch"})
    await hass.async_block_till_done()
    assert hub.visible_cameras() == [("camera.nvr_porch", "Porch"), ("camera.nvr_garden", "Garden")]
    assert hub.resolve_camera("camera.porch") == "camera.nvr_porch"

    with patch("homeassistant.components.camera.async_get_image", return_value=CamImage("image/jpeg", _jpeg())) as get:
        r = await setup.client.get("/api/pebble_home/v1/image?camera=camera.porch&w=100&h=60&bpp=4", headers=H)
        assert r.status == 200
        assert get.call_args.args[1] == "camera.nvr_porch"


# ------------------------------------------------------------------ notify.pebble
PHONE_DATA = {
    "url": "/lovelace-cams/0",
    "entity_id": "camera.porch",
    "actions": [
        {"action": "CALL_TO_UNLOCK_FRONT_DOOR", "title": "Unlock the door", "destructive": True},
        {"action": "URI", "title": "Open Cameras", "uri": "/lovelace-cams/0"},
    ],
}


async def _notify(hass, message, title=None, **data):
    payload = {"message": message, "data": data}
    if title:
        payload["title"] = title
    await hass.services.async_call("notify", "pebble", payload, blocking=True)
    await hass.async_block_till_done()


async def test_notify_service_takes_a_phone_payload(hass, setup):
    assert hass.services.has_service("notify", "pebble")
    await _notify(hass, "Bins tonight", "Home", **PHONE_DATA)
    ev = setup.hub.events[-1]
    assert (ev.title, ev.msg, ev.cam, ev.has_img) == ("Home", "Bins tonight", "camera.porch", True)
    assert [a["action"] for a in ev.actions] == ["CALL_TO_UNLOCK_FRONT_DOOR"], "URI actions can't run on a watch"


async def test_notify_image_url(hass, setup, aioclient_mock):
    aioclient_mock.get("https://example.com/cat.jpg", content=_jpeg(1600, 1200, (10, 200, 10)))
    await _notify(hass, "Look", image="https://example.com/cat.jpg")
    ev = setup.hub.events[-1]
    assert ev.has_img and ev.cam is None
    assert len(setup.hub.images[ev.id]) < 250 * 1024 or Image.open(io.BytesIO(setup.hub.images[ev.id])).width <= 640
    r = await setup.client.get(f"/api/pebble_home/v1/image?event={ev.id}&w=100&h=60&bpp=4", headers=H)
    assert r.status == 200


async def test_notify_unexposed_camera_is_snapshot_only(hass, setup):
    hass.states.async_set("camera.garage", "streaming", {"friendly_name": "Garage"})
    await _notify(hass, "Garage open", camera="camera.garage")
    ev = setup.hub.events[-1]
    assert ev.has_img and ev.cam is None, "the watch would get 403 asking for a live view"


async def test_doorbell_and_its_notification_are_one_alert(hass, setup):
    hass.states.async_set("binary_sensor.doorbell", "on")
    await hass.async_block_till_done()
    await _notify(hass, "Someone at the door", "Front door", **PHONE_DATA)
    assert len(setup.hub.events) == 1
    ev = setup.hub.events[-1]
    assert (ev.kind, ev.vibe, ev.title) == ("doorbell", "doorbell", "Front door")
    assert ev.actions


async def test_notification_first_then_doorbell_is_one_alert(hass, setup):
    await _notify(hass, "Someone at the door", **PHONE_DATA)
    hass.states.async_set("binary_sensor.doorbell", "on")
    await hass.async_block_till_done()
    assert len(setup.hub.events) == 1
    assert setup.hub.events[-1].kind == "doorbell" and setup.hub.events[-1].vibe == "doorbell"


async def test_tag_replaces_and_clear_removes(hass, setup):
    await _notify(hass, "Washer 10 min left", tag="washer")
    await _notify(hass, "Washer done", tag="washer")
    assert [e.msg for e in setup.hub.events] == ["Washer done"]
    await _notify(hass, "clear_notification", tag="washer")
    assert not setup.hub.events


async def test_alert_service_image_and_kind_vibe(hass, setup, aioclient_mock):
    aioclient_mock.get("https://example.com/p.png", content=_jpeg())
    res = await hass.services.async_call(
        "pebble_home", "alert", {"title": "Parcel", "image": "https://example.com/p.png", "kind": "package"},
        blocking=True, return_response=True,
    )
    ev = setup.hub.event(res["id"])
    assert ev.has_img and ev.kind == "package" and ev.vibe == "short"


async def test_local_path_cannot_escape_www(hass, setup):
    from custom_components.pebble_home import sources

    assert await sources.async_fetch(hass, "/local/../secrets.yaml") is None


async def test_notify_camera_down_uses_its_twin(hass, setup):
    hass.states.async_set("camera.nvr_porch", "recording", {"friendly_name": "nvr porch"})
    hass.states.async_set("camera.nvr_garden", "recording", {"friendly_name": "nvr garden"})
    hass.config_entries.async_update_entry(
        setup.entry, options={**setup.entry.options, "cameras": ["camera.porch", "camera.nvr_porch", "camera.nvr_garden"]}
    )
    await hass.async_block_till_done()
    hass.states.async_remove("camera.porch")  # Frigate gone entirely
    await hass.async_block_till_done()
    hub = setup.entry.runtime_data
    with patch("homeassistant.components.camera.async_get_image", return_value=CamImage("image/jpeg", _jpeg())) as get:
        await _notify(hass, "Someone at the door", entity_id="camera.porch", vibe="none")
    ev = hub.events[-1]
    assert ev.has_img and ev.cam == "camera.nvr_porch"
    assert get.call_args.args[1] == "camera.nvr_porch"


async def test_watch_presses_a_notification_button(hass, setup, monkeypatch):
    monkeypatch.setattr("custom_components.pebble_home.http.LOCK_NOTICE_S", 0.2)
    fired = []
    hass.bus.async_listen("mobile_app_notification_action", lambda e: fired.append(e.data))
    await _notify(hass, "Someone at the door", **PHONE_DATA)
    ev = setup.hub.events[-1]
    wire = ev.as_wire()
    assert wire["a"] == "Unlock the door" and wire["af"] == 1
    # Destructive and a PIN is set in HA (fixture): the PIN is checked.
    r = await setup.client.post("/api/pebble_home/v1/action", headers=H, json={"a": "notify", "id": ev.id, "m": 0})
    assert (await r.json())["ok"] is False
    # Launch codes carry only the low 28 bits of the id.
    r = await setup.client.post(
        "/api/pebble_home/v1/action", headers=H,
        json={"a": "notify", "id": ev.id & 0x0FFFFFFF, "m": 0, "code": "4321"},
    )
    assert (await r.json())["ok"] is True
    await hass.async_block_till_done()
    assert fired and fired[-1]["action"] == "CALL_TO_UNLOCK_FRONT_DOOR"
    r = await setup.client.post("/api/pebble_home/v1/action", headers=H, json={"a": "notify", "id": ev.id, "m": 5})
    assert (await r.json())["err"] == "Notification expired"


def _answer_with_lock(hass, *states):
    """Stand-in for the phone automation: on the button, drive lock.front_door through states."""
    from homeassistant.core import Context

    async def run(event):
        ctx = Context(parent_id=event.context.id)  # automations run in a child context
        for i, st in enumerate(states):
            await asyncio.sleep(0.05)
            hass.states.async_set("lock.front_door", st, {"friendly_name": "Front door"}, context=ctx if i == 0 else None)

    async def on_button(event):
        hass.async_create_task(run(event))

    hass.bus.async_listen("mobile_app_notification_action", on_button)


async def _press_unlock(hass, setup):
    await _notify(hass, "Someone at the door", **PHONE_DATA)
    ev = setup.hub.events[-1]
    r = await setup.client.post(
        "/api/pebble_home/v1/action", headers=H, json={"a": "notify", "id": ev.id, "m": 0, "code": "4321"}
    )
    return await r.json()


async def test_notification_button_reports_the_lock_really_opening(hass, setup):
    hass.states.async_set("lock.front_door", "locked", {"friendly_name": "Front door"})
    _answer_with_lock(hass, "unlocking", "unlocked")
    assert await _press_unlock(hass, setup) == {"ok": True, "speech": "Front door unlocked"}


async def test_notification_button_reports_a_lock_that_failed(hass, setup):
    # TTLock: "unlocking", then every transport fails and it reports locked again.
    hass.states.async_set("lock.front_door", "locked", {"friendly_name": "Front door"})
    _answer_with_lock(hass, "unlocking", "locked")
    assert await _press_unlock(hass, setup) == {"ok": False, "err": "Front door did not unlock"}
