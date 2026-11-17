"""A stand-in for the Pebble Home integration, for the emulator and demos.

    python tools/mock_server.py --images DIR      # DIR holds <camera>.jpg files
    curl -X POST localhost:8765/mock/doorbell     # ring the bell
    curl -X POST localhost:8765/mock/alarm/armed_away

Serves the same /api/pebble_home/v1 API with fake but plausible state and
real image conversion, so screenshots look like the real thing.
"""
import argparse, asyncio, base64, json, pathlib, sys, time

from aiohttp import web

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "custom_components" / "pebble_home"))
import imaging  # noqa: E402

KEY = "demo"
LABELS = {"disarmed": "Disarmed", "armed_away": "Armed away", "armed_home": "Armed home",
          "armed_night": "Armed night", "pending": "Entry delay", "arming": "Arming", "triggered": "TRIGGERED"}

S = {
    "v": 1,
    "alarm": "disarmed",
    "by": "Alex",
    "front": "locked",
    "open": ["Kitchen Bifolds"],
    "events": [],
    "media": [
        {"id": "media_player.kitchen", "n": "Kitchen", "s": "playing", "t": "Penny Lane", "a": "The Beatles", "vol": 22, "mute": 0, "lead": 1, "grp": 1, "art": "a1"},
        {"id": "media_player.lounge", "n": "Lounge", "s": "paused", "t": "Gold (Acoustic)", "a": "Kiiara", "vol": 30, "mute": 0, "lead": 1, "grp": 1, "art": "b2"},
        {"id": "media_player.garden", "n": "Garden", "s": "idle", "t": "", "a": "", "vol": 60, "mute": 0, "lead": 1, "grp": 1, "art": ""},
    ],
}
COND = asyncio.Condition()
IMAGES: dict[str, bytes] = {}
SEQ = [int(time.time())]


async def bump():
    S["v"] += 1
    async with COND:
        COND.notify_all()


def state():
    return {
        "v": S["v"], "now": int(time.time()),
        "alarm": {"s": S["alarm"], "l": LABELS.get(S["alarm"], S["alarm"]), "r": True, "by": S["by"], "t": int(time.time()) - 600},
        "ppl": [{"n": "Alex", "h": True}, {"n": "Sam", "h": True}, {"n": "Jo", "h": True}, {"n": "Max", "h": False}],
        "locks": [{"id": "lock.front_door", "n": "Front door", "s": S["front"]}],
        "open": S["open"], "open_n": len(S["open"]),
        "wx": {"c": "partlycloudy", "t": 5, "u": "°C"},
        "st": [{"n": "Solar", "s": "0W"}, {"n": "Lounge", "s": "20.5°C"}, {"n": "Gas today", "s": "4.2kWh"}],
        "cams": [{"id": f"camera.{n}", "n": n.title()} for n in IMAGES],
        "acts": [{"id": "script.good_night", "n": "Good night"}, {"id": "scene.movie", "n": "Movie time"}],
        "media": S["media"], "pin_lock": 0, "last": S["events"][-1] if S["events"] else None,
    }


def auth(req):
    if req.headers.get("X-Pebble-Key") != KEY:
        raise web.HTTPUnauthorized


async def sync(req):
    auth(req)
    v = int(req.query.get("v", -1))
    wait = float(req.query.get("wait", 0))
    if wait and v == S["v"]:
        async with COND:
            try:
                await asyncio.wait_for(COND.wait_for(lambda: S["v"] != v), wait)
            except TimeoutError:
                pass
    e = int(req.query.get("e", 0))
    evs = [x for x in S["events"] if x["id"] > e] if e else []
    return web.json_response({"state": state(), "events": evs[-5:]})


async def events(req):
    auth(req)
    return web.json_response({"events": list(reversed(S["events"]))[:15]})


async def image(req):
    auth(req)
    q = req.query
    src = None
    if "camera" in q:
        src = IMAGES.get(q["camera"].split(".", 1)[-1])
    elif "event" in q:
        ev = next((x for x in S["events"] if str(x["id"]) == q["event"]), None)
        src = IMAGES.get((ev or {}).get("c", "").split(".", 1)[-1])
    elif "media" in q:
        src = IMAGES.get("art") or next(iter(IMAGES.values()), None)
    if not src:
        raise web.HTTPNotFound
    blob = imaging.convert(src, int(q.get("w", 260)), int(q.get("h", 170)), int(q.get("bpp", 4)), q.get("fit", "auto"))
    if q.get("b64"):
        return web.Response(text=base64.b64encode(blob).decode())
    return web.Response(body=blob)


async def hello(req):
    auth(req)
    print("hello", await req.json())
    return web.json_response({"state": state(), "events": S["events"][-1:]})


async def action(req):
    auth(req)
    b = await req.json()
    print("action", b)
    if b["a"] == "alarm":
        if b["m"] == "disarm" and b.get("code") != "1234":
            return web.json_response({"ok": False, "err": "Wrong PIN"})
        S["alarm"] = {"disarm": "disarmed"}.get(b["m"], b["m"].replace("arm_", "armed_"))
        await add_event("alarm", "Alarm " + LABELS[S["alarm"]].lower(), "by Pebble", None, "short")
    elif b["a"] == "lock":
        if b["m"] == "unlock" and b.get("code") != "1234":
            return web.json_response({"ok": False, "err": "Wrong PIN"})
        S["front"] = "locked" if b["m"] == "lock" else "unlocked"
        await bump()
    elif b["a"] == "media":
        m = next(x for x in S["media"] if x["id"] == b["id"])
        if b["m"] == "play_pause":
            m["s"] = "paused" if m["s"] == "playing" else "playing"
        elif b["m"] in ("vol_up", "vol_down"):
            m["vol"] = max(0, min(100, m["vol"] + (4 if b["m"] == "vol_up" else -4)))
        elif b["m"] == "next":
            m["t"], m["a"], m["art"] = "Strawberry Fields Forever", "The Beatles", str(time.time())[-6:]
        await bump()
        return web.json_response({"ok": True, "quiet": True})
    elif b["a"] == "assist":
        return web.json_response({"ok": True, "speech": f"Turned on the kitchen lights. You said: {b.get('text')}"})
    return web.json_response({"ok": True})


async def add_event(kind, title, msg, cam, vibe):
    SEQ[0] += 1
    S["events"].append({"id": SEQ[0], "ts": int(time.time()), "k": kind, "t": title, "m": msg, "c": cam, "v": vibe, "i": 1 if cam else 0})
    await bump()


async def mock_doorbell(req):
    await add_event("doorbell", "Doorbell", "Porch", "camera.porch", "doorbell")
    return web.Response(text="ding dong\n")


async def mock_parcel(req):
    await add_event("package", "Parcel", "Driveway", "camera.driveway", "short")
    return web.Response(text="parcel\n")


async def mock_alarm(req):
    S["alarm"] = req.match_info["state"]
    await bump()
    return web.Response(text=S["alarm"] + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--images", required=True)
    ap.add_argument("--port", type=int, default=8765)
    a = ap.parse_args()
    for p in pathlib.Path(a.images).glob("*.jpg"):
        IMAGES[p.stem] = p.read_bytes()
    app = web.Application()
    base = "/api/pebble_home/v1"
    app.add_routes([web.get(base + "/sync", sync), web.get(base + "/events", events), web.get(base + "/image", image),
                    web.post(base + "/hello", hello), web.post(base + "/action", action),
                    web.post("/mock/doorbell", mock_doorbell), web.post("/mock/parcel", mock_parcel),
                    web.post("/mock/alarm/{state}", mock_alarm)])
    web.run_app(app, port=a.port)


if __name__ == "__main__":
    main()
