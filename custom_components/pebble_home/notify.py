"""Notifications to the watch.

* ``notify.pebble`` takes the same ``data:`` as a mobile_app notification
  (image, entity_id camera, tag, clear_notification...), so it can sit next to
  your phone in the same automation, or in a notify group.
* ``notify.pebble_watch`` (entity) for ``notify.send_message``: title and text.
"""

from __future__ import annotations

import logging
from typing import Any

from homeassistant.components.notify import (
    ATTR_DATA,
    ATTR_TITLE,
    BaseNotificationService,
    NotifyEntity,
)
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback
from homeassistant.helpers.typing import ConfigType, DiscoveryInfoType

from .const import DOMAIN
from .entity import PebbleEntity

_LOGGER = logging.getLogger(__name__)


async def async_setup_entry(
    hass: HomeAssistant, entry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    async_add_entities([PebbleNotify(entry.runtime_data, "notify")])


class PebbleNotify(PebbleEntity, NotifyEntity):
    async def async_send_message(self, message: str, title: str | None = None) -> None:
        await self.hub.async_notify(message, title)


async def async_get_service(
    hass: HomeAssistant, config: ConfigType, discovery_info: DiscoveryInfoType | None = None
) -> PebbleNotificationService:
    return PebbleNotificationService()


class PebbleNotificationService(BaseNotificationService):
    async def async_send_message(self, message: str = "", **kwargs: Any) -> None:
        entries = self.hass.config_entries.async_loaded_entries(DOMAIN)
        if not entries:
            _LOGGER.warning("notify.pebble called but Pebble Home is not set up")
            return
        await entries[0].runtime_data.async_notify(
            message, kwargs.get(ATTR_TITLE), kwargs.get(ATTR_DATA) or {}
        )
