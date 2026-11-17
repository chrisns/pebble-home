#!/usr/bin/env bash
# Put new public builds of the watchface and watchapp into the integration's
# appstore feed (custom_components/pebble_home/watch/), with one version for
# the integration and both watch apps. The Pebble app picks the builds up from
# the feed once the new integration version is running in Home Assistant.
#
#   tools/release.sh 1.2        # then commit, tag v1.2.0, push
set -euo pipefail
cd "$(dirname "$0")/.."
V=${1:-}
[[ "$V" =~ ^[0-9]+\.[0-9]+$ ]] || { echo "usage: tools/release.sh <major.minor>   (the watch only keeps major.minor)"; exit 1; }
python3 - "$V.0" <<'PY'
import json, sys
v = sys.argv[1]
for path in ("watchapp/package.json", "watchface/package.json", "custom_components/pebble_home/manifest.json"):
    d = json.load(open(path))
    d["version"] = v
    open(path, "w").write(json.dumps(d, indent=2) + "\n")
PY
STORE=1 ./build.sh
W=custom_components/pebble_home/watch
cp watchapp/build/watchapp.pbw "$W/Home.pbw"
cp watchface/build/watchface.pbw "$W/HomeFace.pbw"
# Icons and screenshots (<App>-icon.png, <App>-shotN.png) live in $W and change by hand.
python3 tools/check_store.py "$W"
# The local sideload builds carry your settings again.
./build.sh >/dev/null
echo "Store builds $V ready in $W. Commit, tag v$V.0 and push."
