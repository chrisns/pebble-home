"""Write dev_config.js with demo mode on and real images pre-converted.

    python tools/make_demo.py DIR [--doorbell-after 20]

DIR holds <camera>.jpg files and optionally art.jpg (album art).
"""
import argparse, base64, json, pathlib, sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "custom_components" / "pebble_home"))
import imaging  # noqa: E402

SIZES = [(260, 260), (260, 146), (260, 170), (120, 120)]

ap = argparse.ArgumentParser()
ap.add_argument("dir")
ap.add_argument("--doorbell-after", type=float, default=0)
ap.add_argument("--parcel-after", type=float, default=0)
ap.add_argument("--pin", action="store_true", help="demo HA has a watch PIN (1234)")
ap.add_argument("--arm-after", type=float, default=0, help="start arming away after N seconds")
a = ap.parse_args()
images = {}
for p in sorted(pathlib.Path(a.dir).glob("*.jpg")):
    src = p.read_bytes()
    for w, h in SIZES:
        fit = "cover" if (w, h) != (260, 146) else "auto"
        images[f"{p.stem}_{w}x{h}"] = base64.b64encode(imaging.convert(src, w, h, 4, fit)).decode()
script = []
if a.doorbell_after:
    script.append({"after": a.doorbell_after, "k": "doorbell", "t": "Doorbell", "m": "Porch", "c": "camera.porch", "v": "doorbell"})
if a.parcel_after:
    script.append({"after": a.parcel_after, "k": "package", "t": "Parcel", "m": "Driveway", "c": "camera.driveway", "v": "short"})
if a.arm_after:
    script.append({"after": a.arm_after, "arm": "away"})
cams = sorted({k.rsplit("_", 1)[0] for k in images})
demo = {"images": images, "script": script, "cams": cams, "pin": a.pin}
root = pathlib.Path(__file__).resolve().parents[1]
(root / "dev_config.js").write_text("module.exports = { demo: " + json.dumps(demo) + " };\n")
print(f"dev_config.js: {len(images)} images, {len(script)} scripted events")
