"""Config and options flow."""

from homeassistant import config_entries
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType

from custom_components.pebble_home.const import DOMAIN


async def test_user_flow_guesses_and_creates(hass: HomeAssistant):
    hass.states.async_set("alarm_control_panel.house", "disarmed")
    hass.states.async_set("person.chris", "home")
    hass.states.async_set("lock.front", "locked")
    hass.states.async_set("weather.home", "sunny", {"temperature": 10})
    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_USER})
    assert result["type"] is FlowResultType.FORM
    schema = {str(k): k for k in result["data_schema"].schema}
    assert schema["alarm_entity"].default() == "alarm_control_panel.house"
    assert schema["people"].default() == ["person.chris"]

    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {"alarm_entity": "alarm_control_panel.house", "watch_pin": "12a4"}
    )
    assert result["errors"] == {"watch_pin": "pin_digits"}

    result = await hass.config_entries.flow.async_configure(
        result["flow_id"], {"alarm_entity": "alarm_control_panel.house", "watch_pin": "1234", "people": ["person.chris"]}
    )
    assert result["type"] is FlowResultType.CREATE_ENTRY
    key = result["data"]["api_key"]
    assert len(key) >= 20
    assert result["options"]["watch_pin"] == "1234"
    await hass.async_block_till_done()

    # Single instance only.
    again = await hass.config_entries.flow.async_init(DOMAIN, context={"source": config_entries.SOURCE_USER})
    assert again["type"] is FlowResultType.ABORT

    # Options flow shows the key and saves changes.
    entry = hass.config_entries.async_entries(DOMAIN)[0]
    opt = await hass.config_entries.options.async_init(entry.entry_id)
    assert opt["description_placeholders"]["key"] == key
    opt = await hass.config_entries.options.async_configure(
        opt["flow_id"], {"alarm_entity": "alarm_control_panel.house", "watch_pin": "9999"}
    )
    assert opt["type"] is FlowResultType.CREATE_ENTRY
    await hass.async_block_till_done()
    assert entry.options["watch_pin"] == "9999"
