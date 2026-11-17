"""The appstore feed the Pebble app reads: shape, update pickup, and nothing but watch/ served."""

from __future__ import annotations

import json
import os
import struct
import zipfile
from urllib.parse import unquote

import pytest

from custom_components.pebble_home.const import DOMAIN

from .test_integration import setup  # noqa: F401  (fixture)

FACE_UUID = "6a1b0c55-1111-4b7c-9d1e-00000000face"
APP_UUID = "6a1b0c55-2222-4b7c-9d1e-0000000000a9"
BASE = "/api/pebble_home/store"

# Fields StoreApplication declares without a default: a card missing one is dropped by the app.
REQUIRED = {
    "author", "category", "category_color", "category_id", "changelog", "companions", "compatibility",
    "created_at", "description", "developer_id", "header_images", "hearts", "icon_image", "id", "links",
    "list_image", "published_date", "screenshot_hardware", "screenshot_images", "source", "title", "type",
    "visible", "website",
}


def _pbw(path, uuid, name, face, version):
    head = bytearray(100)
    head[:8] = b"PBLAPP\0\0"
    head[10:12] = bytes([5, 110])
    head[24:24 + len(name)] = name.encode()
    struct.pack_into("<I", head, 88, 1)
    struct.pack_into("<I", head, 96, 0x1C9 if face else 0x1C8)
    info = {
        "uuid": uuid, "longName": name, "shortName": name, "versionLabel": version, "companyName": "Someone",
        "targetPlatforms": ["gabbro"], "watchapp": {"watchface": face}, "capabilities": ["configurable"],
        "configPage": "src/config.html", "manifestVersion": 2,
        "usesPermissions": [{"name": "LocalNetwork", "reason": "HA", "required": False}],
    }
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("appinfo.json", json.dumps(info))
        z.writestr("gabbro/pebble-app.bin", bytes(head) + b"code")


@pytest.fixture
def watch_dir(hass, setup, tmp_path):  # noqa: F811
    _pbw(tmp_path / "HomeFace.pbw", FACE_UUID, "Home Face", True, "1.1")
    _pbw(tmp_path / "Home.pbw", APP_UUID, "Home", False, "1.1")
    (tmp_path / "Home-icon.png").write_bytes(b"\x89PNG icon")
    (tmp_path / "Home-shot1.png").write_bytes(b"\x89PNG shot")
    hass.data[DOMAIN]["store"].folder = tmp_path
    return tmp_path


async def test_page_offers_one_tap_add(hass, setup, watch_dir):  # noqa: F811
    r = await setup.client.get(BASE)
    assert r.status == 200
    text = await r.text()
    assert "pebble://add-store-feed/Pebble%20Home/" in text
    link = text.split("pebble://add-store-feed/Pebble%20Home/")[1].split("'")[0]
    assert unquote(link).endswith(BASE)
    assert "Home Face 1.1" in text


async def test_home_lists_faces_and_apps_separately(hass, setup, watch_dir):  # noqa: F811
    faces = await (await setup.client.get(f"{BASE}/v1/home/faces?platform=ios&hardware=gabbro")).json()
    apps = await (await setup.client.get(f"{BASE}/v1/home/apps?platform=ios&hardware=gabbro")).json()
    assert [a["title"] for a in faces["applications"]] == ["Home Face"]
    assert [a["title"] for a in apps["applications"]] == ["Home"]
    cat = faces["categories"][0]
    assert {"application_ids", "color", "icon", "id", "links", "name", "slug"} <= cat.keys()
    assert faces["collections"][0]["application_ids"] == [faces["applications"][0]["id"]]


async def test_card_matches_the_binary(hass, setup, watch_dir):  # noqa: F811
    apps = await (await setup.client.get(f"{BASE}/v1/home/apps?hardware=gabbro")).json()
    app_id = apps["applications"][0]["id"]
    one = await (await setup.client.get(f"{BASE}/v1/apps/id/{app_id}?platform=ios&hardware=gabbro")).json()
    c = one["data"][0]
    assert REQUIRED <= c.keys()
    assert c["uuid"] == APP_UUID and c["type"] == "watchapp"
    assert c["latest_release"]["version"] == "1.1"
    assert c["compatibility"]["gabbro"]["supported"] and not c["compatibility"]["basalt"]["supported"]
    hw = c["hardware_platforms"][0]
    assert hw == {**hw, "name": "gabbro", "sdk_version": "5.110", "pebble_process_info_flags": 0x1C8}
    assert c["config_page"] == "src/config.html" and c["capabilities"] == ["configurable"]
    assert c["screenshot_images"] == [{"260x260": f"http://127.0.0.1:{setup.client.port}{BASE}/img/Home-shot1.png"}]
    # The build it points at is the one in watch/.
    pbw = await setup.client.get(c["latest_release"]["pbw_file"].split(str(setup.client.port))[1])
    assert pbw.status == 200 and await pbw.read() == (watch_dir / "Home.pbw").read_bytes()


async def test_bulk_update_check_sees_a_new_build_without_restart(hass, setup, watch_dir):  # noqa: F811
    apps = await (await setup.client.get(f"{BASE}/v1/apps/dev/x")).json()
    ids = [a["id"] for a in apps["data"]]
    r = await setup.client.post(f"{BASE}/v1/apps/bulk", json={"ids": [*ids, "f" * 24]})
    body = await r.json()
    assert {a["latest_release"]["version"] for a in body["data"]} == {"1.1"}
    assert body["missing"] == ["f" * 24]
    # A new integration version drops new builds in watch/: the next check sees them.
    _pbw(watch_dir / "Home.pbw", APP_UUID, "Home", False, "1.2")
    os.utime(watch_dir / "Home.pbw", (1, 2_000_000_000))
    body = await (await setup.client.post(f"{BASE}/v1/apps/bulk", json={"ids": ids})).json()
    assert sorted(a["latest_release"]["version"] for a in body["data"]) == ["1.1", "1.2"]


async def test_store_serves_nothing_outside_watch_dir(hass, setup, watch_dir):  # noqa: F811
    (watch_dir.parent / "secret.png").write_bytes(b"no")
    for path in ("img/..%2Fsecret.png", "img/secret.png", "img/Home-icon.png%00", "pbw/../Home.pbw/x"):
        r = await setup.client.get(f"{BASE}/{path}")
        assert r.status in (400, 404), path
    assert (await setup.client.get(f"{BASE}/img/Home-icon.png")).status == 200
    assert (await setup.client.post(f"{BASE}/v1/apps/bulk", data="nope")).status == 400


def test_shipped_builds_carry_no_settings():
    """The builds in the integration are public: the settings module must be the empty one."""
    import re

    from custom_components.pebble_home.store import STORE_DIR

    pbws = sorted(STORE_DIR.glob("*.pbw"))
    assert {p.name for p in pbws} == {"Home.pbw", "HomeFace.pbw"}
    for pbw in pbws:
        with zipfile.ZipFile(pbw) as z:
            js = z.read("pebble-js-app.js").decode()
        assert re.search(r"module\.exports\s*=\s*\{\s*\}\s*;", js), pbw.name
        assert not re.search(r"\b(url|remote|key)\s*:\s*['\"][^'\"]+['\"]", js.split("module.exports = {};")[0][-400:])
