"""Pebble Home: Home Assistant on your Pebble."""

from __future__ import annotations

import voluptuous as vol

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import Platform
from homeassistant.core import HomeAssistant, ServiceCall, SupportsResponse
from homeassistant.helpers import config_validation as cv, discovery
from homeassistant.helpers.typing import ConfigType

from .const import DOMAIN, KIND_ALERT, VIBES
from . import store
from .http import VIEWS
from .hub import PebbleHub

PLATFORMS = [Platform.BUTTON, Platform.NOTIFY, Platform.SENSOR]

type PebbleConfigEntry = ConfigEntry[PebbleHub]

CONFIG_SCHEMA = cv.config_entry_only_config_schema(DOMAIN)

ALERT_SCHEMA = vol.Schema(
    {
        vol.Required("title"): cv.string,
        vol.Optional("message", default=""): cv.string,
        vol.Optional("camera"): cv.entity_id,
        vol.Optional("image"): cv.string,
        vol.Optional("vibe"): vol.In(VIBES),
        vol.Optional("kind", default=KIND_ALERT): cv.string,
    }
)


async def async_setup(hass: HomeAssistant, config: ConfigType) -> bool:
    hass.data[DOMAIN] = {"config": config}
    for view in VIEWS:
        hass.http.register_view(view())
    hass.data[DOMAIN]["store"] = store.register(hass)  # the watch apps, as a Pebble appstore source

    async def _alert(call: ServiceCall):
        entries = hass.config_entries.async_loaded_entries(DOMAIN)
        if not entries:
            return {"id": None}
        hub: PebbleHub = entries[0].runtime_data
        data = {k: call.data[k] for k in ("camera", "image", "vibe", "kind") if k in call.data}
        ev = await hub.async_notify(call.data["message"], call.data["title"], data)
        return {"id": ev.id if ev else None}

    hass.services.async_register(
        DOMAIN, "alert", _alert, schema=ALERT_SCHEMA, supports_response=SupportsResponse.OPTIONAL
    )
    return True


async def async_setup_entry(hass: HomeAssistant, entry: PebbleConfigEntry) -> bool:
    hub = PebbleHub(hass, entry)
    entry.runtime_data = hub
    await hub.async_start()
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    if not hass.data[DOMAIN].get("notify_service"):
        # notify.pebble, the rich one. Legacy-style like mobile_app's, because
        # notify entities only take a title and a message.
        hass.data[DOMAIN]["notify_service"] = True
        hass.async_create_task(
            discovery.async_load_platform(
                hass, Platform.NOTIFY, DOMAIN, {"name": "pebble"}, hass.data[DOMAIN]["config"]
            )
        )
    entry.async_on_unload(entry.add_update_listener(_reload))
    return True


async def _reload(hass: HomeAssistant, entry: PebbleConfigEntry) -> None:
    await hass.config_entries.async_reload(entry.entry_id)


async def async_unload_entry(hass: HomeAssistant, entry: PebbleConfigEntry) -> bool:
    await entry.runtime_data.async_stop()
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
