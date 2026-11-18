# Pebble Home

Home Assistant on a Pebble Round 2 (`gabbro`, 260×260 colour, round). Three parts:

| Part | What it does |
|---|---|
| `custom_components/pebble_home` | Home Assistant integration. Watches the house, keeps an event feed with snapshots, converts camera frames and album art into bitmaps the watch draws directly, and exposes a small, keyed API. Adds `notify.pebble` (takes phone-style `data:` with images), `notify.pebble_watch`, `pebble_home.alert`, battery and last-seen sensors, and a PIN lockout reset button. |
| `watchface/` | **Home Face.** Time ringed by the alarm state (green disarmed, amber home, violet night, red away, blinking for entry and exit delay). Weather, latest event or now playing, front lock, open doors and windows, and who is home. When the doorbell goes, the face becomes the porch camera, refreshing live. Flick your wrist to dismiss it. |
| `watchapp/` | **Home.** Alarm arm and disarm (PIN), Sonos music with album art and a volume ring, live cameras, event history with snapshots, locks, quick actions, Assist by voice, and status. |

## How alerts reach the watch

On iOS a watch app's phone-side JavaScript only runs while that app is in front, and
iPhone notifications reach the watch with no images and no custom buttons. So:

1. **The face pulls.** It asks the phone for a sync every 60 s, or every 15 s during
   entry or exit delay, after an event, or while an alert is showing. Each request
   wakes the Pebble phone app over Bluetooth, which iOS allows even when the phone is
   locked. If the phone app happens to be awake, a long-poll makes updates instant.
2. **Events become timeline pins.** Selecting a pin's *View snapshot* action opens
   the face on that snapshot.
3. **Keep your iPhone push** (`notify.mobile_app_*`) for the instant buzz. The watch adds
   the picture.

> Not yet verified on a real watch with the phone locked. If iOS throttles the
> wake-ups, alerts arrive at the next poll rather than instantly.

## Security

- The phone holds a **per-install watch key**, never a Home Assistant token. It can
  only do what this integration exposes: listed cameras, players, locks and actions.
- **PIN is optional.** Set one in Home Assistant (checked server side) or in the
  phone settings page (checked by the phone before the request leaves it). With no
  PIN, disarming and unlocking take a second press within 3 seconds, so a stray click
  can't do it, but anyone holding the watch can. Visonic in Powerlink mode (like many
  panels) disarms with any code or none, so the panel is never the check.
- Wrong PINs: 3 in a row lock code actions for 30 s, then 60 s, 2 min and so on.
  In Home Assistant, 10 in a row lock them until you press **Reset PIN lockout**,
  and the count survives restarts.
- The key goes in a header only, never in a URL, so it doesn't end up in proxy logs.

## Install

### Home Assistant

1. **HACS → ⋮ → Custom repositories**, add `https://github.com/chrisns/pebble-home` as an
   *Integration*, install **Pebble Home** and restart. (Or copy
   `custom_components/pebble_home` to `/config/custom_components/` and restart.)
2. **Settings → Devices & services → Add integration → Pebble Home.** The form is
   pre-filled with guesses: alarm, Frigate cameras, Sonos players, people, locks, and
   door and window sensors. A watch PIN is optional.
3. Copy the **watch key** from the success dialog. It's also shown under *Configure*.

### Watch: from your Home Assistant's own appstore source

The integration serves the watchface and watchapp as a source for the Pebble app, so
they install and update like any store app, and a HACS update of the integration is
also the update of the watch apps.

1. On the phone, open `https://<your-ha>/api/pebble_home/store` (the exact link is under
   the integration's *Configure*) and tap **Add to the Pebble app**. Or in the Pebble app:
   **Settings → Apps → Appstore Sources → Add Source**, and paste that address.
   Use the address that works away from home (Nabu Casa or your external URL): the
   phone keeps it.
2. Install **Home Face** and **Home** from the new *Pebble Home* source.
3. Open **Settings** for each of them in the Pebble app and enter your HA URL (and the
   outside one, if any) and the watch key. Phone storage is per app, so they don't share it.

The source serves only public things: app details, icons, and builds with nothing of
yours inside. It needs no login, and the watch key is never in it.

**Doorbell from the watchface.** A watchface never gets buttons (Select always opens the
launcher), so set **Settings → Quick Launch → Tap Up / Tap Down** on the watch to **Home**.
During a doorbell alert, Up or Down then opens Home and unlocks the door. Inside Home, Up,
Select and Down all unlock; the alert shows live video from the camera (about one frame a
second over Bluetooth) and closes 45 s after the ring. The pill says **Unlocked** only once
Home Assistant has seen the lock open.

### Watch: building it yourself

```bash
uv tool install pebble-tool --python 3.13
pebble sdk install latest
./build.sh                        # builds watchface/ and watchapp/
HA_DOMAIN=ha.example.com ./build.sh   # also declares Internet access to your HA up front
```

Install `watchface/build/watchface.pbw` and `watchapp/build/watchapp.pbw` by opening them
on the phone, or with `tools/push.sh <phone-ip>` while developer connection is on. With
**Settings → Plugins → Plugins / permissions preview** turned on you get the bundled
settings page; without it the same form opens through the older settings flow.

For a sideload with the connection pre-filled, create an untracked `local_config.js`:

```js
module.exports = { url: 'https://ha.example.com', key: 'the-watch-key' };
```

### Releasing

`tools/release.sh 1.2` sets one version on the integration and both watch apps, builds
them with `STORE=1` (nothing from `local_config.js`), copies them into
`custom_components/pebble_home/watch/`, and runs `tools/check_store.py`, which fails if
the key, your URLs or your phone's address are in either build. Commit, tag `v1.2.0`,
push, and publish a GitHub release so HACS offers it. The watch only keeps major.minor,
so bump at least the minor number.

## Using it in automations

`notify.pebble` takes the same `data:` as a phone notification, so the watch can
sit next to your phone in the same automation, or in a notify group:

```yaml
action: notify.pebble
data:
  title: Doorbell
  message: Someone at the door
  data:
    entity_id: camera.porch      # snapshot now, live view on the watch
    # image: /api/frigate/notifications/<id>/snapshot.jpg   or /local/x.jpg,
    #        media-source://..., image.front_door, https://...
    tag: door                    # a later one with the same tag replaces it
    vibe: doorbell               # none | short | double | long | doorbell | alarm
    kind: doorbell               # alert | doorbell | person | package | alarm
```

- Phone-only keys (`url`, `push`, `group`, `URI` actions) are ignored, so you can
  paste a mobile_app `data:` block as is. `message: clear_notification` with a
  `tag` removes it.
- iOS `push: {interruption-level: passive}` arrives silently; `critical` buzzes like
  the alarm.
- **One press, one buzz.** If the doorbell automation also sends `notify.pebble`, it
  joins the doorbell event this integration raises by itself (same press within 20 s,
  same camera or `kind: doorbell`). Your title, message and picture win.
- `pebble_home.alert` takes the same `image`, and also returns the event id.
- `notify.send_message` on the `notify.pebble_watch` entity does title and text only.

Without any automation, these raise events on their own:

- doorbell presses
- alarm state changes
- Frigate `person` and `package` events inside a zone, when Frigate is enabled. These
  are logged always but buzz only while armed; you can change that.

> Speed: the face fetches new events on its next sync, every 60 s (15 s around an
> event), or straight away while the phone's Pebble app is awake. Keep the phone
> notification for the instant buzz; the watch brings the picture.

## Buttons

| Screen | Up / Down | Select | Hold |
|---|---|---|---|
| Music | volume ± 4% (hold to repeat) | play / pause | Select: next track. Up/Down: previous / next speaker |
| Cameras | previous / next camera | full circle ↔ widescreen | |
| Alarm | choose action | do it (Disarm asks for the PIN) | |
| PIN | digit ± | next digit | Back: undo |
| Face alert | | | Flick wrist: dismiss |

## Development

```bash
uv venv .venv && uv pip install --python .venv/bin/python pytest-homeassistant-custom-component PyTurboJPEG
.venv/bin/python -m pytest          # integration tests against a real HA core

# Emulator with no Home Assistant at all: demo data and your own images
.venv/bin/python tools/make_demo.py path/to/jpgs --doorbell-after 25
DEMO=1 ./build.sh && (cd watchface && pebble install --emulator gabbro)

.venv/bin/python tools/mock_server.py --images path/to/jpgs    # or a local fake API
```

`tools/pbimg_preview.py` decodes the wire format back to PNG, exactly as the watch draws it.

### Image wire format

The integration does the heavy lifting: crop, auto-contrast, pick a 16-colour palette
from the Pebble 64-colour gamut, Floyd–Steinberg dither, and pack rows MSB-first. A
260×260 frame is 34 KB at 4 bpp, sent in 6 KB AppMessage chunks.

## Known limits

- Snapshots live in Home Assistant's memory, so a restart clears them. Pins outlive
  their images.
- iPhone notifications on the watch can't carry images or custom actions. That's iOS,
  not this.
- The Pebble plugin API (complications on any face, actions from Index) would suit
  this well but is still a preview. It's a natural next step.
