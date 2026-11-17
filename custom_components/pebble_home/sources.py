"""Turn whatever an automation hands us (camera, URL, path, media id) into image bytes."""

from __future__ import annotations

import asyncio
from datetime import timedelta
import io
import logging
from pathlib import Path
import re

from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.network import NoURLAvailableError, get_url

_LOGGER = logging.getLogger(__name__)

MAX_BYTES = 8 * 1024 * 1024
KEEP_BYTES = 250 * 1024  # bigger than this gets shrunk before it is kept in memory
FETCH_TIMEOUT = 10
_CAMERA_PROXY = re.compile(r"^/api/camera_proxy(?:_stream)?/(camera\.[a-z0-9_]+)")


def camera_from(value: str | None) -> str | None:
    """'camera.porch' or '/api/camera_proxy/camera.porch?token=..' -> 'camera.porch'."""
    if not value:
        return None
    if value.startswith("camera."):
        return value
    m = _CAMERA_PROXY.match(value)
    return m.group(1) if m else None


def _local_base(hass: HomeAssistant) -> str:
    try:
        return get_url(hass, allow_external=False, allow_cloud=False, prefer_external=False)
    except NoURLAvailableError:
        port = getattr(hass.http, "server_port", None) or 8123
        return f"http://127.0.0.1:{port}"


async def async_fetch(hass: HomeAssistant, src: str) -> bytes | None:
    """Bytes for an image reference, or None. Never raises."""
    try:
        return await _fetch(hass, src.strip())
    except Exception as err:  # noqa: BLE001
        _LOGGER.warning("Pebble Home could not load image %s: %s", src, err)
        return None


async def _fetch(hass: HomeAssistant, src: str) -> bytes | None:
    if src.startswith("image."):
        src = f"/api/image_proxy/{src}"
    if src.startswith("media-source://"):
        from homeassistant.components import media_source

        src = (await media_source.async_resolve_media(hass, src, None)).url

    if src.startswith("/local/"):
        www = Path(hass.config.path("www")).resolve()
        path = (www / src[len("/local/"):].split("?")[0]).resolve()
        if not path.is_relative_to(www):
            _LOGGER.warning("Pebble Home refused image path outside www: %s", src)
            return None
        return await hass.async_add_executor_job(_read_file, path)

    if src.startswith("/") and hass.config.is_allowed_path(src) and not src.startswith("/api/"):
        return await hass.async_add_executor_job(_read_file, Path(src))

    if src.startswith("/"):
        if "authSig=" not in src:
            from homeassistant.components.http.auth import async_sign_path

            src = async_sign_path(hass, src, timedelta(minutes=1), use_content_user=True)
        url = _local_base(hass) + src
    elif src.startswith(("http://", "https://")):
        url = src
    else:
        _LOGGER.warning("Pebble Home does not know how to load image %s", src)
        return None

    session = async_get_clientsession(hass, verify_ssl=False)
    async with asyncio.timeout(FETCH_TIMEOUT):
        async with session.get(url) as resp:
            if resp.status != 200:
                _LOGGER.warning("Pebble Home image %s returned HTTP %s", src.split("?")[0], resp.status)
                return None
            data = await resp.content.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        _LOGGER.warning("Pebble Home image %s is over %d bytes", src.split("?")[0], MAX_BYTES)
        return None
    return data


def _read_file(path: Path) -> bytes | None:
    if not path.is_file() or path.stat().st_size > MAX_BYTES:
        return None
    return path.read_bytes()


def shrink(data: bytes) -> bytes:
    """Keep memory small: the watch never needs more than ~520 px of anything."""
    if len(data) <= KEEP_BYTES:
        return data
    from PIL import Image, ImageOps

    try:
        img = Image.open(io.BytesIO(data))
        img = ImageOps.exif_transpose(img).convert("RGB")
        img.thumbnail((640, 640))
        out = io.BytesIO()
        img.save(out, "JPEG", quality=85)
        return out.getvalue()
    except Exception:  # noqa: BLE001
        return data
