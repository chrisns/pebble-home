"""Config flow: pick what the watch shows, with sensible guesses from the house."""

from __future__ import annotations

import secrets
from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigEntry, ConfigFlow, ConfigFlowResult, OptionsFlow
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers import selector as sel

from . import store
from .const import (
    CONF_ACTIONS,
    CONF_AGENT,
    CONF_ALARM,
    CONF_ALARM_CODE,
    CONF_CAMERAS,
    CONF_DOORBELL,
    CONF_DOORBELL_CAMERA,
    CONF_ENTRY_DELAY,
    CONF_EXIT_DELAY,
    DEFAULT_DELAY,
    CONF_FRIGATE,
    CONF_FRIGATE_LABELS,
    CONF_FRIGATE_VIBE,
    CONF_FRIGATE_ZONES_ONLY,
    CONF_KEY,
    CONF_LOCKS,
    CONF_MEDIA,
    CONF_PEOPLE,
    CONF_PERIMETER,
    CONF_PIN,
    CONF_STATUS,
    CONF_WEATHER,
    DEFAULT_FRIGATE_LABELS,
    DOMAIN,
    FRIGATE_VIBE_ALWAYS,
    FRIGATE_VIBE_ARMED,
    FRIGATE_VIBE_NEVER,
)

OPENING_CLASSES = {"door", "window", "garage_door", "opening"}


def _guess(hass: HomeAssistant) -> dict[str, Any]:
    """Best guesses so the first form is mostly a confirm."""
    states = hass.states.async_all()
    ok = [s for s in states if s.state not in ("unavailable", "unknown")]
    reg = er.async_get(hass)

    def platform(entity_id: str) -> str | None:
        ent = reg.async_get(entity_id)
        return ent.platform if ent else None

    alarms = [s.entity_id for s in ok if s.domain == "alarm_control_panel"]
    cams = [s.entity_id for s in ok if s.domain == "camera" and platform(s.entity_id) == "frigate"]
    if not cams:
        cams = [s.entity_id for s in ok if s.domain == "camera"][:8]
    doorbell_cam = next((c for c in cams if any(k in c for k in ("door", "porch", "bell"))), None)
    doorbells = [
        s.entity_id
        for s in states
        if s.domain in ("binary_sensor", "event")
        and "doorbell" in s.entity_id
        and "battery" not in s.entity_id
    ]
    perimeter = [
        s.entity_id
        for s in states
        if s.domain == "binary_sensor"
        and s.attributes.get("device_class") in OPENING_CLASSES
        and (platform(s.entity_id) in ("visonic", "zwave_js", "zha", "mqtt") or "alarm" in s.entity_id)
    ]
    return {
        CONF_ALARM: alarms[0] if alarms else None,
        CONF_CAMERAS: cams,
        CONF_DOORBELL_CAMERA: doorbell_cam,
        CONF_DOORBELL: doorbells[:2],
        CONF_PEOPLE: [s.entity_id for s in states if s.domain == "person"],
        CONF_LOCKS: [s.entity_id for s in ok if s.domain == "lock"],
        CONF_PERIMETER: perimeter[:40],
        CONF_MEDIA: [
            s.entity_id for s in ok if s.domain == "media_player" and platform(s.entity_id) == "sonos"
        ][:12],
        CONF_WEATHER: next((s.entity_id for s in ok if s.domain == "weather"), None),
        CONF_FRIGATE: "frigate" in hass.config.components and "mqtt" in hass.config.components,
    }


def _schema(defaults: dict[str, Any]) -> vol.Schema:
    def d(key: str, fallback: Any = None) -> dict[str, Any]:
        val = defaults.get(key, fallback)
        return {"default": val} if val not in (None, "") else {}

    multi = lambda domain, **kw: sel.EntitySelector(  # noqa: E731
        sel.EntitySelectorConfig(domain=domain, multiple=True, **kw)
    )
    single = lambda domain: sel.EntitySelector(sel.EntitySelectorConfig(domain=domain))  # noqa: E731
    return vol.Schema(
        {
            vol.Optional(CONF_ALARM, **d(CONF_ALARM)): single("alarm_control_panel"),
            vol.Optional(CONF_DOORBELL, **d(CONF_DOORBELL, [])): multi(
                ["binary_sensor", "event", "input_boolean"]
            ),
            vol.Optional(CONF_DOORBELL_CAMERA, **d(CONF_DOORBELL_CAMERA)): single("camera"),
            vol.Optional(CONF_CAMERAS, **d(CONF_CAMERAS, [])): multi("camera"),
            vol.Optional(CONF_PEOPLE, **d(CONF_PEOPLE, [])): multi("person"),
            vol.Optional(CONF_LOCKS, **d(CONF_LOCKS, [])): multi("lock"),
            vol.Optional(CONF_PERIMETER, **d(CONF_PERIMETER, [])): multi("binary_sensor"),
            vol.Optional(CONF_MEDIA, **d(CONF_MEDIA, [])): multi("media_player"),
            vol.Optional(CONF_WEATHER, **d(CONF_WEATHER)): single("weather"),
            vol.Optional(CONF_STATUS, **d(CONF_STATUS, [])): sel.EntitySelector(
                sel.EntitySelectorConfig(multiple=True)
            ),
            vol.Optional(CONF_ACTIONS, **d(CONF_ACTIONS, [])): multi(
                ["script", "scene", "button", "input_button", "automation", "switch", "light", "input_boolean"]
            ),
            vol.Optional(CONF_AGENT, **d(CONF_AGENT)): sel.ConversationAgentSelector(),
            vol.Optional(CONF_FRIGATE, **d(CONF_FRIGATE, False)): bool,
            vol.Optional(CONF_FRIGATE_LABELS, **d(CONF_FRIGATE_LABELS, DEFAULT_FRIGATE_LABELS)): sel.SelectSelector(
                sel.SelectSelectorConfig(
                    options=["person", "package", "car", "dog", "cat", "bicycle", "motorcycle"],
                    multiple=True,
                    custom_value=True,
                )
            ),
            vol.Optional(CONF_FRIGATE_ZONES_ONLY, **d(CONF_FRIGATE_ZONES_ONLY, True)): bool,
            vol.Optional(CONF_FRIGATE_VIBE, **d(CONF_FRIGATE_VIBE, FRIGATE_VIBE_ARMED)): sel.SelectSelector(
                sel.SelectSelectorConfig(
                    options=[FRIGATE_VIBE_ARMED, FRIGATE_VIBE_ALWAYS, FRIGATE_VIBE_NEVER],
                    translation_key="frigate_vibe",
                )
            ),
            vol.Optional(CONF_EXIT_DELAY, default=defaults.get(CONF_EXIT_DELAY, DEFAULT_DELAY)): sel.NumberSelector(
                sel.NumberSelectorConfig(min=0, max=180, step=1, unit_of_measurement="s", mode=sel.NumberSelectorMode.BOX)
            ),
            vol.Optional(CONF_ENTRY_DELAY, default=defaults.get(CONF_ENTRY_DELAY, DEFAULT_DELAY)): sel.NumberSelector(
                sel.NumberSelectorConfig(min=0, max=180, step=1, unit_of_measurement="s", mode=sel.NumberSelectorMode.BOX)
            ),
            vol.Optional(CONF_ALARM_CODE, **d(CONF_ALARM_CODE)): sel.TextSelector(
                sel.TextSelectorConfig(type=sel.TextSelectorType.PASSWORD)
            ),
            vol.Optional(CONF_PIN, **d(CONF_PIN)): sel.TextSelector(
                sel.TextSelectorConfig(type=sel.TextSelectorType.PASSWORD)
            ),
        }
    )


def _clean(user_input: dict[str, Any]) -> dict[str, Any]:
    pin = str(user_input.get(CONF_PIN) or "")
    if pin and not pin.isdigit():
        raise ValueError("pin_digits")
    return {k: v for k, v in user_input.items() if v not in (None, "")}


class PebbleHomeConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            try:
                options = _clean(user_input)
            except ValueError as err:
                errors[CONF_PIN] = str(err)
            else:
                key = secrets.token_urlsafe(18)
                return self.async_create_entry(
                    title="Pebble",
                    data={CONF_KEY: key},
                    options=options,
                    description_placeholders={"key": key},
                )
        return self.async_show_form(
            step_id="user",
            data_schema=_schema(user_input or _guess(self.hass)),
            errors=errors,
        )

    @staticmethod
    @callback
    def async_get_options_flow(config_entry: ConfigEntry) -> OptionsFlow:
        return PebbleHomeOptionsFlow()


class PebbleHomeOptionsFlow(OptionsFlow):
    async def async_step_init(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            try:
                return self.async_create_entry(data=_clean(user_input))
            except ValueError as err:
                errors[CONF_PIN] = str(err)
        return self.async_show_form(
            step_id="init",
            data_schema=_schema(user_input or dict(self.config_entry.options)),
            errors=errors,
            description_placeholders={"key": self.config_entry.data[CONF_KEY], "store": store.source_url(self.hass)},
        )
