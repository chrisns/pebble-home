#!/usr/bin/env python3
"""Fail if a store build carries anything of yours: the watch key, your URLs, your phone.

Reads the values from local_config.js and .pushrc (never printed) and looks for
them in every file inside every .pbw given, or in a folder of them. Also checks
the baked-in settings module is the empty one a STORE=1 build writes.
"""

import re
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def secrets() -> set[bytes]:
    found: set[str] = set()
    lc = ROOT / "local_config.js"
    if lc.exists():
        found |= {v for v in re.findall(r"['\"]([^'\"]{6,})['\"]", lc.read_text())}
    rc = ROOT / ".pushrc"
    if rc.exists():
        found |= {v.strip() for v in re.findall(r"=(.+)", rc.read_text()) if len(v.strip()) >= 6}
    hosts = {re.sub(r"^https?://", "", s).split("/")[0] for s in found if s.startswith("http")}
    return {s.encode() for s in found | hosts if s}


def main(args: list[str]) -> int:
    paths = []
    for a in args or [str(ROOT / "custom_components/pebble_home/watch")]:
        p = Path(a)
        paths += sorted(p.glob("*.pbw")) if p.is_dir() else [p]
    if not paths:
        print("check_store: no .pbw found")
        return 1
    bad = 0
    needles = secrets()
    for pbw in paths:
        with zipfile.ZipFile(pbw) as z:
            for name in z.namelist():
                data = z.read(name)
                if any(n in data for n in needles):
                    print(f"check_store: {pbw.name}:{name} contains one of your private values")
                    bad += 1
            js = z.read("pebble-js-app.js").decode("utf-8", "replace")
            if not re.search(r"module\.exports\s*=\s*\{\s*\}\s*;", js):
                print(f"check_store: {pbw.name} does not carry the empty settings module (build with STORE=1)")
                bad += 1
    if not bad:
        print(f"check_store: {len(paths)} build(s) clean ({len(needles)} private values checked)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
