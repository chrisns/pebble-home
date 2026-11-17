"""Sensors the watch reports back: battery and last contact."""

from __future__ import annotations

from datetime import UTC, datetime

from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorStateClass,
)
from homeassistant.const import PERCENTAGE, EntityCategory
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .entity import PebbleEntity
from .hub import SIGNAL_WATCH_UPDATE


async def async_setup_entry(
    hass: HomeAssistant, entry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    hub = entry.runtime_data
    async_add_entities([BatterySensor(hub, "battery"), LastSeenSensor(hub, "last_seen")])


class _WatchSensor(PebbleEntity, SensorEntity):
    async def async_added_to_hass(self) -> None:
        self.async_on_remove(
            async_dispatcher_connect(self.hass, SIGNAL_WATCH_UPDATE, self._updated)
        )

    @callback
    def _updated(self) -> None:
        self.async_write_ha_state()


class BatterySensor(_WatchSensor):
    _attr_device_class = SensorDeviceClass.BATTERY
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_native_unit_of_measurement = PERCENTAGE

    @property
    def native_value(self) -> int | None:
        return self.hub.watch.battery

    @property
    def extra_state_attributes(self):
        return {"charging": self.hub.watch.charging, "app": self.hub.watch.app}


class LastSeenSensor(_WatchSensor):
    _attr_device_class = SensorDeviceClass.TIMESTAMP
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    @property
    def native_value(self) -> datetime | None:
        ts = self.hub.watch.last_seen
        return datetime.fromtimestamp(ts, UTC) if ts else None
