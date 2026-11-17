"""Constants for Pebble Home."""

DOMAIN = "pebble_home"

CONF_KEY = "api_key"
CONF_ALARM = "alarm_entity"
CONF_ALARM_CODE = "alarm_code"
CONF_PIN = "watch_pin"
CONF_CAMERAS = "cameras"
CONF_DOORBELL = "doorbell_entities"
CONF_DOORBELL_CAMERA = "doorbell_camera"
CONF_PEOPLE = "people"
CONF_LOCKS = "locks"
CONF_PERIMETER = "perimeter"
CONF_WEATHER = "weather"
CONF_STATUS = "status_entities"
CONF_ACTIONS = "quick_actions"
CONF_AGENT = "assist_agent"
CONF_MEDIA = "media_players"
CONF_EXIT_DELAY = "exit_delay"
CONF_ENTRY_DELAY = "entry_delay"
DEFAULT_DELAY = 30
CONF_FRIGATE = "frigate_events"
CONF_FRIGATE_LABELS = "frigate_labels"
CONF_FRIGATE_VIBE = "frigate_vibrate"
CONF_FRIGATE_ZONES_ONLY = "frigate_zones_only"

FRIGATE_VIBE_ALWAYS = "always"
FRIGATE_VIBE_ARMED = "armed"
FRIGATE_VIBE_NEVER = "never"

DEFAULT_FRIGATE_LABELS = ["person", "package"]

# Event kinds. The watch picks an icon and colour per kind.
KIND_DOORBELL = "doorbell"
KIND_PERSON = "person"
KIND_PACKAGE = "package"
KIND_ALARM = "alarm"
KIND_ALERT = "alert"

# Vibration patterns understood by the watch.
VIBES = ["none", "short", "double", "long", "doorbell", "alarm"]

MAX_EVENTS = 30
MAX_WAIT = 50
DOORBELL_DEBOUNCE = 20
NOTIFY_JOIN = 5  # a notification this close before a doorbell press is that press
PIN_HARD_LOCK = 10

API_BASE = "/api/pebble_home/v1"
