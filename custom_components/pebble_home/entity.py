"""Shared entity base."""

from __future__ import annotations

from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity import Entity

from .const import DOMAIN
from .hub import PebbleHub


class PebbleEntity(Entity):
    _attr_has_entity_name = True
    _attr_should_poll = False

    def __init__(self, hub: PebbleHub, key: str) -> None:
        self.hub = hub
        self._attr_unique_id = f"{hub.entry.entry_id}_{key}"
        self._attr_translation_key = key
        self._attr_device_info = DeviceInfo(
            identifiers={(DOMAIN, hub.entry.entry_id)},
            name="Pebble",
            manufacturer="Core Devices",
            model=hub.watch.model or "Pebble Round 2",
            sw_version=hub.watch.firmware,
        )
