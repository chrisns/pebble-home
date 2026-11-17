#!/usr/bin/env bash
# Push the watchface and watchapp to the real watch over the Pebble app's
# developer connection. Waits until the phone is reachable, so run it, then
# open the Pebble app (Dev Connection on) and keep it in front for ~10 s.
#
#   tools/push.sh [phone-ip]       # default: $PEBBLE_PHONE or .pushrc
set -euo pipefail
cd "$(dirname "$0")/.."
[[ -f .pushrc ]] && source .pushrc
PHONE=${1:-${PEBBLE_PHONE:-}}
[[ -n "$PHONE" ]] || { echo "usage: tools/push.sh <phone-ip> (or set PEBBLE_PHONE in .pushrc)"; exit 1; }
export PYTHONWARNINGS=ignore
PEBBLE=${PEBBLE:-$HOME/.local/bin/pebble}
WAIT=${WAIT:-900}
echo "Waiting up to ${WAIT}s for the Pebble app's dev connection on $PHONE:9000..."
for ((t = 0; t < WAIT; t += 3)); do
  if nc -z -G 2 "$PHONE" 9000 2>/dev/null; then
    for p in watchface watchapp; do
      echo "Installing $p..."
      "$PEBBLE" install --phone "$PHONE" "$p/build/$p.pbw" 2>&1 | grep -E "succeeded|failed|rror" || true
    done
    exit 0
  fi
  sleep 3
done
echo "Phone never appeared. Is Dev Connection on and the Pebble app in front?"
exit 1
