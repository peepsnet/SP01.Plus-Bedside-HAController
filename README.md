# SP01.Plus-Bedside-HAController

Customizable Home Assistant bedside touchscreen for the **WT32-SC01 Plus** (ESP32-S3, 3.5" 480x320 capacitive touch), built with PlatformIO, Arduino, LovyanGFX and LVGL 9.

> **Status: v0.1.0.** It compiles with `pio run`. **Nothing has been tested on real hardware yet.** Items marked *(guess)* are from published board pinouts or my assumptions and need a check on the device.

## What it does

- Draws pages of touch buttons from `/layout.json` on the device's flash (LittleFS).
- Connects to Home Assistant over the WebSocket API with a long-lived access token. It subscribes to the entities used in the layout (`subscribe_entities`), so buttons light up when the entity is `on`, and sends `call_service` when tapped (toggle, on, off, scene, script, press).
- Serves a layout editor at `http://bedside.local/` (add, drag, resize buttons, set label, icon, entity id, action, color, multiple pages). Saving applies instantly with no reflash.
- Serves a settings page at `/settings` for Wi-Fi and Home Assistant host/token. Secrets are never sent back to the browser.
- If Wi-Fi is not configured or fails, it opens an AP named `Bedside-Setup`; browse to `http://192.168.4.1/settings`. *(The AP is open, no password. Fine for first setup, tighten later.)*

## Setup

1. Install [PlatformIO](https://platformio.org/).
2. In Home Assistant: your profile page, Security, **Long-lived access tokens**, create one.
3. Flash the firmware: see "Installing the firmware" below (web flasher or `pio run -t upload`).
4. Either configure on the device, or pre-load config:
   - Copy `data/config.example.json` to `data/config.json` (gitignored), fill it in, then `pio run -t uploadfs`.
   - Or skip that: join the `Bedside-Setup` Wi-Fi after first boot and fill in `/settings`.
5. Open `http://bedside.local/`, edit your buttons, hit **Save & apply**.

`data/layout.json` is the default layout uploaded by `uploadfs`. Note `uploadfs` overwrites the layout on the device, so use it only for first setup.

## Web login

The web UI uses HTTP basic auth: user `admin`, password generated on first boot. Find it on the touchscreen: tap the gear (bottom right), the Settings screen shows `Login: admin / <password>`. It is stored in `/config.json` on the device. This is HTTP, not HTTPS: it keeps casual LAN visitors out but passwords can be sniffed on the network.

## Installing the firmware

Release files come in two flavours. **Use the right one:**

| File | What it is | Use it for |
|---|---|---|
| `SC01Plus-Bedside-HAController-vX.Y.Z-full.bin` | bootloader + partition table + app, one image | **First install**, flash at offset `0x0` |
| `SC01Plus-Bedside-HAController-vX.Y.Z-ota.bin` | app only | Updates through the web `/update` page or the touchscreen |

Flashing the `-ota.bin` (app-only) image at `0x0` leaves the board in a boot loop (`SHA-256 comparison failed ... Attempting to boot anyway`, `TG0WDT_SYS_RST`), because the ROM treats the app as the bootloader. Recovery is just flashing the `-full.bin` at `0x0`.

**Web flasher (easiest):** open the project's GitHub Pages site (`https://peepsnet.github.io/SP01.Plus-Bedside-HAController/`, once Pages is enabled and a release has been published), click Install, then enter your Wi-Fi network when asked (Improv Wi-Fi provisioning over serial). Chrome or Edge on a desktop is required.

**esptool:** `esptool.py --chip esp32s3 --baud 460800 write_flash 0x0 SC01Plus-Bedside-HAController-vX.Y.Z-full.bin`

**From source:** `pio run -t upload` writes all parts at the right offsets. `pio run` also creates both release files in `dist/`.

## Versions and releases

- The version lives in one place: the `VERSION` file. `scripts/version.py` compiles it into the firmware; it is shown on the editor/update pages and the touchscreen Settings screen.
- **Releasing:** on GitHub go to **Actions, release, Run workflow** on `main` and choose patch, minor or major. The workflow bumps `VERSION`, builds, commits `Release vX.Y.Z` to `main`, tags it, creates the GitHub release with the two `.bin` files, and then starts the `pages` workflow to update the web flasher. No git commands needed. (If `main` is branch-protected, allow the Actions bot to push, or the commit step fails.)
- One-time setup for the flasher: **Settings, Pages, Source: GitHub Actions**, then run **Actions, pages, Run workflow** once (it publishes the latest release's full image). Later releases update it automatically.
- The `build` workflow compiles every push and PR and uploads the `.bin` files as an artifact.

## Updating the firmware (OTA)

The 16 MB partition table (`default_16MB.csv`) has two app slots, so updates are OTA-safe.

- **Web:** `http://bedside.local/update`, choose the `-ota.bin`, upload. The device reboots.
- **Touchscreen:** gear, **Check update** (asks GitHub for the latest release), then **Install** downloads that release's `-ota.bin` and flashes it.

Caveats: the on-device GitHub update does not verify TLS certificates and images are not signed, and it only works if the repository is public. All OTA paths are *untested on hardware*; keep USB access handy.

## Controls and Home Assistant addresses

- **Buttons:** toggle, on, off, scene, script, press, cover open/close/stop, lock/unlock, media play/pause. Buttons light up when the entity is on/open/playing/unlocked/heating.
- **Sliders** (add with "+ Add slider" in the editor): light brightness, light color temperature (Kelvin), cover position, fan speed, media volume, climate target temperature, `number` and `input_number`. Sliders follow the entity's live value and send while you drag (at most every 300 ms) and on release. Optional `min`/`max` override the defaults. RGB color wheels and HVAC mode pickers are not done yet.
- **Entity picker:** the editor loads your devices from Home Assistant (lights, switches, covers, fans, climate, media players, scenes, scripts, helpers, locks, buttons) so you can pick them by name; typing an id still works. It needs the Home Assistant connection to be up.
- **Local and external URL:** Settings has a local address (tried first) and an optional external address, for example a Nabu Casa or reverse-proxy name. If the local one does not authenticate within 12 s the device switches to the external one, and while on external it retries local every 5 minutes.

## layout.json

```json
{"pages":[{"name":"Bedroom","buttons":[
  {"id":"b1","label":"Lamp","icon":"power","x":10,"y":10,"w":150,"h":90,
   "entity_id":"light.bedroom_lamp","action":"toggle","color":"#F5A623"},
  {"id":"b2","type":"slider","label":"Brightness","x":170,"y":10,"w":220,"h":70,
   "entity_id":"light.bedroom_lamp","attr":"brightness","color":"#F5A623"}
]}]}
```

Coordinates are pixels in a 480x280 area (the bottom 40 px is the page tab/status bar). Button actions: `toggle`, `on`, `off`, `scene`, `script`, `press`, `open`, `close`, `stop`, `lock`, `unlock`, `play_pause`. Sliders use `"type":"slider"` with optional `attr` (`brightness` or `color_temp` for lights), `min` and `max`. Icons: `power, eye, home, play, pause, bell, settings, ok, wifi, audio, charge, image` (LVGL built-in symbols).

## Layout of the code

| File | Purpose |
|---|---|
| `src/display.h` | LovyanGFX pins for ST7796 + FT6336 *(guess, standard published values)* |
| `src/ui.cpp` | LVGL 9 setup, builds buttons from the layout |
| `src/ha_client.cpp` | HA WebSocket auth, subscribe, service calls |
| `src/web.cpp`, `src/web_pages.h` | HTTP API + editor and settings pages |
| `src/config.cpp` | Settings and layout storage |
| `src/improv.cpp` | Improv Wi-Fi serial provisioning *(untested)* |
| `scripts/` | version injection, release-image merge, version bump, flasher manifest |
| `site/` | web flasher page (ESP Web Tools) |

## Known gaps and guesses

- All hardware behavior: pins, rotation (`setRotation(1)`; use 3 if upside down), touch axes, backlight, PSRAM setting (`qio_qspi`, 16 MB flash) are *(guess)* and untested.
- `subscribe_entities` message parsing (`a` snapshot and `c` change formats) follows my reading of HA's frontend protocol; it compiles but is untested against a live Home Assistant.
- TLS (`wss`) path is untested and does not verify certificates.
- No authentication on the web editor: anyone on your LAN can edit the layout or settings. Add auth before exposing beyond a trusted network.
- No entity picker yet; entity ids are typed in. Next ideas: fetch the entity list from HA, sliders/dimmers, sensor tiles, screen dimming/sleep, OTA updates.
