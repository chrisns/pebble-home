#!/usr/bin/env bash
# Build the watchface and the watchapp. Shared code lives in shared/ and is
# copied in, so each Pebble project stays a normal, self-contained project.
set -euo pipefail
cd "$(dirname "$0")"
export PYTHONWARNINGS=ignore
PEBBLE=${PEBBLE:-$HOME/.local/bin/pebble}
targets=${*:-watchface watchapp}
for proj in $targets; do
  rm -rf "$proj/src/c/shared"
  mkdir -p "$proj/src/c/shared" "$proj/src/pkjs"
  cp shared/c/*.c shared/c/*.h "$proj/src/c/shared/"
  cp shared/pkjs/hass.js shared/pkjs/demo.js "$proj/src/pkjs/"
  cp shared/web/config.html "$proj/src/config.html"
  # The phone side tells Home Assistant which build is on the watch, so the
  # face can say when the integration ships a newer one.
  python3 -c 'import json,sys;print("module.exports = " + json.dumps(json.load(open(sys.argv[1]))["version"]) + ";")' \
    "$proj/package.json" > "$proj/src/pkjs/version.js"
  # The same page as a JS string, for phones without the plugins preview,
  # where the app falls back to the legacy openURL settings flow.
  python3 -c 'import json,sys;print("module.exports = " + json.dumps(open(sys.argv[1]).read()) + ";")' \
    shared/web/config.html > "$proj/src/pkjs/config_page.js"
  # DEMO=1 bakes in the offline demo (dev_config.js from tools/make_demo.py).
  # Otherwise local_config.js (untracked: { url, key }) pre-fills the
  # connection for a sideload, or nothing is baked in and the settings page
  # does it.
  # STORE=1 is the public build for the integration's appstore feed: nothing
  # of yours baked in, so each watch is set up on the settings page.
  if [[ "${STORE:-0}" == 1 ]]; then echo "module.exports = {};" > "$proj/src/pkjs/dev_config.js"
  elif [[ "${DEMO:-0}" == 1 ]]; then cp dev_config.js "$proj/src/pkjs/dev_config.js"
  elif [[ -f local_config.js ]]; then cp local_config.js "$proj/src/pkjs/dev_config.js"
  else echo "module.exports = {};" > "$proj/src/pkjs/dev_config.js"; fi
  # HA_DOMAIN=ha.example.com,xxxx.ui.nabu.casa declares Internet access to your Home Assistant
  # up front (otherwise the Pebble app asks on first use). Not committed.
  cp "$proj/package.json" "$proj/.package.json.orig"
  trap 'for p in watchface watchapp; do [[ -f $p/.package.json.orig ]] && mv $p/.package.json.orig $p/package.json; done' EXIT
  if [[ -n "${HA_DOMAIN:-}" && "${STORE:-0}" != 1 ]]; then
    python3 - "$proj/package.json" "$HA_DOMAIN" <<'PY'
import json, sys
path, domain = sys.argv[1], sys.argv[2]
d = json.load(open(path))
d["pebble"]["usesPermissions"].insert(0, {"name": "Internet", "parameters": {"domains": [x.strip() for x in domain.split(",") if x.strip()]},
    "reason": "To reach your Home Assistant", "required": True})
json.dump(d, open(path, "w"), indent=2)
PY
  fi
  (cd "$proj" && "$PEBBLE" build 2>&1 | grep -v -i "warning: \|SyntaxWarning\|^  :param\|^  Represents" )
  mv "$proj/.package.json.orig" "$proj/package.json"
done
