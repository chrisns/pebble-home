"""A Pebble appstore feed for the watchface and watchapp, served by this integration.

The Pebble app reads apps from any number of appstore "sources". Adding this
one means the watch apps arrive, and update, the same way the integration
does: a new version of the integration carries new builds in ``watch/``, and
the Pebble app sees the higher version on its next locker refresh.

Everything here is public: app details, icons and builds without any key.
The key is entered once on the watch app's settings page, never baked in.
The routes follow the feed format the Pebble app parses
(coredevices/mobileapp ``AppstoreService`` / ``StoreApplication``).
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import UTC, datetime
from email.utils import formatdate
import hashlib
import html
import json
import logging
from pathlib import Path
import struct
from typing import Any
from urllib.parse import quote
import zipfile

from aiohttp import web

from homeassistant.components.http import HomeAssistantView
from homeassistant.core import HomeAssistant
from homeassistant.helpers.network import NoURLAvailableError, get_url

from .const import DOMAIN

_LOGGER = logging.getLogger(__name__)

STORE_DIR = Path(__file__).parent / "watch"
STORE_BASE = f"/api/{DOMAIN}/store"  # the source address people add
STORE_NAME = "Pebble Home"
DEVELOPER_ID = hashlib.md5(b"pebble_home developer").hexdigest()[:24]
CATEGORY = {"id": hashlib.md5(b"pebble_home category").hexdigest()[:24], "name": "Home Assistant",
            "slug": "home-assistant", "color": "18bcf2"}
COLLECTION = "pebble-home"
WATCH_PLATFORMS = ("aplite", "basalt", "chalk", "diorite", "emery", "flint", "gabbro")
SCREEN = {"aplite": "144x168", "basalt": "144x168", "diorite": "144x168", "flint": "144x168",
          "chalk": "180x180", "emery": "200x228", "gabbro": "260x260"}
BLURB = {
    "watchface": "Your home at a glance: alarm ring, locks, weather, and doorbell and camera "
    "alerts with live video. Needs the Pebble Home integration in Home Assistant.",
    "watchapp": "Arm and disarm the alarm, unlock the door from a doorbell alert, watch "
    "cameras live, control speakers and ask Assist. Needs the Pebble Home integration "
    "in Home Assistant.",
}


@dataclass(frozen=True)
class StoreApp:
    """One .pbw in watch/, read once per change of the file."""

    file: str
    id: str
    uuid: str
    title: str
    type: str  # watchface | watchapp
    version: str
    author: str
    published: str
    platforms: dict[str, dict[str, Any]]  # name -> sdk_version, flags, icon resource id
    capabilities: list[str]
    config_page: str | None
    permissions: list[dict[str, Any]]
    manifest_version: int | None
    icons: list[str]  # files in watch/, "<stem>-*.png"


def _read_pbw(path: Path) -> StoreApp | None:
    try:
        with zipfile.ZipFile(path) as z:
            info = json.loads(z.read("appinfo.json"))
            platforms = {}
            for plat in info.get("targetPlatforms", []):
                if plat not in WATCH_PLATFORMS:
                    continue
                try:
                    head = z.read(f"{plat}/pebble-app.bin")[:100]
                except KeyError:
                    continue
                if len(head) < 100 or head[:8] != b"PBLAPP\0\0":
                    continue
                # PebbleProcessInfo: sdk_version at 10, icon_resource_id at 88, flags at 96.
                platforms[plat] = {
                    "sdk_version": f"{head[10]}.{head[11]}",
                    "flags": struct.unpack_from("<I", head, 96)[0],
                    "icon": struct.unpack_from("<I", head, 88)[0],
                }
    except (OSError, KeyError, ValueError, zipfile.BadZipFile) as err:
        _LOGGER.warning("Pebble Home store skips %s: %s", path.name, err)
        return None
    if not platforms or not info.get("uuid"):
        return None
    face = bool(info.get("watchapp", {}).get("watchface"))
    stamp = datetime.fromtimestamp(path.stat().st_mtime, UTC).strftime("%Y-%m-%dT%H:%M:%S")
    return StoreApp(
        file=path.name,
        id=hashlib.md5(info["uuid"].lower().encode()).hexdigest()[:24],
        uuid=info["uuid"].lower(),
        title=info.get("longName") or info.get("shortName") or path.stem,
        type="watchface" if face else "watchapp",
        version=str(info.get("versionLabel") or "1.0"),
        author=info.get("companyName") or STORE_NAME,
        published=stamp,
        platforms=platforms,
        capabilities=[c for c in info.get("capabilities", []) if c in ("configurable", "location", "health")],
        config_page=info.get("configPage"),
        permissions=info.get("usesPermissions") or [],
        manifest_version=info.get("manifestVersion"),
        icons=sorted(p.name for p in path.parent.glob(f"{path.stem}-*.png")),
    )


class Store:
    """The apps in watch/. Re-read only when a file there changes, so new builds need no restart."""

    def __init__(self, hass: HomeAssistant, folder: Path = STORE_DIR) -> None:
        self.hass = hass
        self.folder = folder
        self._seen: tuple = ()
        self._apps: list[StoreApp] = []

    def _scan(self) -> tuple:
        if not self.folder.is_dir():
            return ()
        return tuple(sorted((p.name, p.stat().st_mtime_ns) for p in self.folder.iterdir() if p.is_file()))

    async def apps(self) -> list[StoreApp]:
        seen = await self.hass.async_add_executor_job(self._scan)
        if seen != self._seen:
            pbws = sorted(self.folder.glob("*.pbw")) if seen else []
            found = [await self.hass.async_add_executor_job(_read_pbw, p) for p in pbws]
            self._apps = [a for a in found if a]
            self._seen = seen
        return self._apps

    async def app(self, app_id: str) -> StoreApp | None:
        return next((a for a in await self.apps() if a.id == app_id), None)


def _origin(request: web.Request) -> str:
    """The address the phone used, so every link in a card works from where it asked."""
    proto = request.headers.get("X-Forwarded-Proto", request.scheme).split(",")[0].strip()
    host = request.headers.get("X-Forwarded-Host", request.host).split(",")[0].strip()
    return f"{proto}://{host}"


def card(app: StoreApp, origin: str, hardware: str | None) -> dict[str, Any]:
    """A ``StoreApplication`` as the Pebble app parses it. Every non-optional field is present."""
    base = origin + STORE_BASE
    plat = hardware if hardware in app.platforms else next(iter(app.platforms))
    shots = [f"{base}/img/{n}" for n in app.icons if "-shot" in n]
    icon = next((f"{base}/img/{n}" for n in app.icons if n.endswith("-icon.png")), "")
    desc = BLURB[app.type]
    notes = f"Version {app.version}"
    release = {
        "id": hashlib.md5(f"{app.id}{app.version}".encode()).hexdigest()[:24],
        "js_md5": None,
        "js_version": -1,
        "pbw_file": f"{base}/pbw/{app.id}/{app.version}.pbw",
        "published_date": app.published,
        "release_notes": notes,
        "settings_page_state": "page_loads" if "configurable" in app.capabilities else "no_page",
        "version": app.version,
    }
    compat: dict[str, Any] = {
        p: {"supported": p in app.platforms, "firmware": {"major": 3}, "has_binary": p in app.platforms}
        for p in WATCH_PLATFORMS
    }
    compat["android"] = {"supported": True}
    compat["ios"] = {"supported": True, "min_js_version": 1}
    return {
        "author": app.author,
        "capabilities": app.capabilities,
        "uses_permissions": app.permissions,
        "manifest_version": app.manifest_version,
        "config_page": app.config_page,
        "category": CATEGORY["name"],
        "category_color": CATEGORY["color"],
        "category_id": CATEGORY["id"],
        "changelog": [{"version": app.version, "published_date": app.published, "release_notes": notes}],
        "companions": {"android": None, "ios": None},
        "compatibility": compat,
        "created_at": app.published,
        "description": desc,
        "developer_id": DEVELOPER_ID,
        "hardware_platforms": [
            {
                "name": p,
                "sdk_version": d["sdk_version"],
                "pebble_process_info_flags": d["flags"],
                "description": desc,
                "images": {"icon": icon, "list": icon, "screenshot": shots[0] if shots else ""},
            }
            for p, d in app.platforms.items()
        ],
        "header_images": None,
        "hearts": 0,
        "icon_image": {"28x28": icon, "48x48": icon} if icon else {},
        "icon_resource_id": app.platforms[plat]["icon"],
        "id": app.id,
        "latest_release": release,
        "links": {},
        "list_image": {"80x80": icon, "144x144": icon} if icon else {},
        "published_date": app.published,
        "screenshot_hardware": plat,
        "screenshot_images": [{SCREEN[plat]: s} for s in shots],
        "source": "https://github.com/chrisns/pebble-home",
        "title": app.title,
        "type": app.type,
        "uuid": app.uuid,
        "visible": True,
        "website": "https://github.com/chrisns/pebble-home",
        "contactable": False,
    }


def _page(apps: list[dict], offset: int = 0) -> dict[str, Any]:
    return {"data": apps, "limit": max(len(apps), 1), "offset": offset, "links": {"nextPage": None}}


def source_url(hass: HomeAssistant, request: web.Request | None = None) -> str:
    """Prefer the address that works away from home: the source is stored on the phone for good."""
    try:
        return get_url(hass, allow_internal=False, prefer_external=True) + STORE_BASE
    except NoURLAvailableError:
        pass
    if request is not None:
        return _origin(request) + STORE_BASE
    try:
        return get_url(hass) + STORE_BASE
    except NoURLAvailableError:
        return STORE_BASE


def add_link(url: str) -> str:
    return f"pebble://add-store-feed/{quote(STORE_NAME, safe='')}/{quote(url, safe='')}"


class _StoreView(HomeAssistantView):
    requires_auth = False  # the Pebble app sends no credentials; nothing here is private
    cors_allowed = True

    def __init__(self, store: Store) -> None:
        self.store = store


class StorePageView(_StoreView):
    """Opened in a browser on the phone: one tap adds the source."""

    url = STORE_BASE
    name = "api:pebble_home:store"

    async def get(self, request: web.Request) -> web.Response:
        hass = request.app["hass"]
        src = source_url(hass, request)
        apps = await self.store.apps()
        rows = "".join(f"<li>{html.escape(a.title)} {html.escape(a.version)}</li>" for a in apps)
        body = (
            "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
            f"<title>{STORE_NAME}</title><style>body{{font:16px/1.5 system-ui,sans-serif;margin:2rem auto;"
            "max-width:34rem;padding:0 1rem}a.b{display:inline-block;padding:.7em 1.2em;background:#18bcf2;"
            "color:#fff;border-radius:8px;text-decoration:none}code{word-break:break-all}</style>"
            f"<h1>{STORE_NAME}</h1><p>The watchface and watchapp for this Home Assistant, as a source "
            "in the Pebble app. Updates arrive with the integration.</p>"
            f"<p><a class=b href='{html.escape(add_link(src))}'>Add to the Pebble app</a></p>"
            "<p>Or in the Pebble app: Settings &rarr; Apps &rarr; Appstore Sources &rarr; Add Source, "
            f"and paste<br><code>{html.escape(src)}</code></p><ul>{rows}</ul>"
        )
        return web.Response(text=body, content_type="text/html")


class StoreHomeView(_StoreView):
    url = STORE_BASE + "/v1/home/{kind}"
    name = "api:pebble_home:store:home"

    async def get(self, request: web.Request, kind: str) -> web.Response:
        want = {"apps": "watchapp", "faces": "watchface"}.get(kind)
        origin, hw = _origin(request), request.query.get("hardware")
        apps = [a for a in await self.store.apps() if want is None or a.type == want]
        ids = [a.id for a in apps]
        links = {"apps": f"{origin}{STORE_BASE}/v1/apps/collection/{COLLECTION}/{kind}"}
        return self.json({
            "applications": [card(a, origin, hw) for a in apps],
            "categories": [{**CATEGORY, "application_ids": ids, "icon": {}, "links": links}],
            "collections": [{"slug": COLLECTION, "name": STORE_NAME, "application_ids": ids, "links": links}],
            "onboarding": {p: [a.id for a in apps if p in a.platforms] for p in WATCH_PLATFORMS},
        })


class StoreListView(_StoreView):
    """/v1/apps/{collection|category|dev}/<slug>[/apps|faces]: there is only ever one list."""

    url = STORE_BASE + "/v1/apps/{kind}/{rest:.*}"
    name = "api:pebble_home:store:list"

    async def get(self, request: web.Request, kind: str, rest: str) -> web.Response:
        origin, hw = _origin(request), request.query.get("hardware")
        if kind == "id":
            app = await self.store.app(rest.strip("/"))
            if app is None:
                return self.json({"error": "app not found"}, status_code=404)
            return self.json(_page([card(app, origin, hw)]))
        if kind not in ("collection", "category", "dev"):
            return self.json({"error": "not found"}, status_code=404)
        want = {"apps": "watchapp", "faces": "watchface"}.get(rest.rstrip("/").rsplit("/", 1)[-1])
        apps = [a for a in await self.store.apps() if want is None or a.type == want]
        raw = request.query.get("offset", "0")
        offset = int(raw) if raw.isdigit() else 0
        return self.json(_page([card(a, origin, hw) for a in apps][offset:], offset))


class StoreBulkView(_StoreView):
    """The Pebble app's update check: POST {"ids": [...]}."""

    url = STORE_BASE + "/v1/apps/bulk"
    name = "api:pebble_home:store:bulk"

    async def post(self, request: web.Request) -> web.Response:
        try:
            body = await request.json()
            ids = [str(i) for i in body.get("ids", [])][:500]
        except (ValueError, AttributeError):
            return self.json({"error": "bad request"}, status_code=400)
        origin, hw = _origin(request), body.get("hardware") if isinstance(body.get("hardware"), str) else None
        by_id = {a.id: a for a in await self.store.apps()}
        return self.json({
            "data": [card(by_id[i], origin, hw) for i in ids if i in by_id],
            "missing": [i for i in ids if i not in by_id],
        })


class StoreFileView(_StoreView):
    """Builds and images, by fixed names from watch/ only."""

    url = STORE_BASE + "/{what:pbw|img}/{name}/{rest:.*}"
    name = "api:pebble_home:store:file"
    extra_urls = [STORE_BASE + "/{what:img}/{name}"]

    async def get(self, request: web.Request, what: str, name: str, rest: str = "") -> web.StreamResponse:
        if what == "pbw":
            app = await self.store.app(name)
            if app is None:
                raise web.HTTPNotFound
            return web.FileResponse(self.store.folder / app.file, headers={
                "Content-Type": "application/octet-stream",
                "Content-Disposition": f'attachment; filename="{app.file}"',
                "Last-Modified": formatdate(usegmt=True),
            })
        files = {n for a in await self.store.apps() for n in a.icons}
        if name not in files:  # never a path from the request
            raise web.HTTPNotFound
        return web.FileResponse(self.store.folder / name, headers={"Content-Type": "image/png",
                                                                    "Cache-Control": "max-age=3600"})


def register(hass: HomeAssistant) -> Store:
    store = Store(hass)
    # The bulk route goes first: "/v1/apps/{kind}/{rest}" would otherwise take it.
    for view in (StoreBulkView, StorePageView, StoreHomeView, StoreListView, StoreFileView):
        hass.http.register_view(view(store))
    return store
