# SP01.Plus-Bedside-HAController

Customizable Home Assistant bedside touchscreen for the **WT32-SC01 Plus** (ESP32-S3, 3.5" 480x320 capacitive touch), built with PlatformIO, Arduino, LovyanGFX and LVGL 9.

> **Status: milestone one.** It compiles with `pio run`. **Nothing has been tested on real hardware yet.** Items marked *(guess)* are from published board pinouts or my assumptions and need a check on the device.

## What it does

- Draws pages of touch buttons from `/layout.json` on the device's flash (LittleFS).
- Connects to Home Assistant over the WebSocket API with a long-lived access token. It subscribes to the entities used in the layout (`subscribe_entities`), so buttons light up when the entity is `on`, and sends `call_service` when tapped (toggle, on, off, scene, script, press).
- Serves a layout editor at `http://bedside.local/` (add, drag, resize buttons, set label, icon, entity id, action, color, multiple pages). Saving applies instantly with no reflash.
- Serves a settings page at `/settings` for Wi-Fi and Home Assistant host/token. Secrets are never sent back to the browser.
- If Wi-Fi is not configured or fails, it opens an AP named `Bedside-Setup`; browse to `http://192.168.4.1/settings`. *(The AP is open, no password. Fine for first setup, tighten later.)*

## Setup

1. Install [PlatformIO](https://platformio.org/).
2. In Home Assistant: your profile page, Security, **Long-lived access tokens**, create one.
3. Build and flash the firmware: `pio run -t upload`
4. Either configure on the device, or pre-load config:
   - Copy `data/config.example.json` to `data/config.json` (gitignored), fill it in, then `pio run -t uploadfs`.
   - Or skip that: join the `Bedside-Setup` Wi-Fi after first boot and fill in `/settings`.
5. Open `http://bedside.local/`, edit your buttons, hit **Save & apply**.

`data/layout.json` is the default layout uploaded by `uploadfs`. Note `uploadfs` overwrites the layout on the device, so use it only for first setup.

## layout.json

```json
{"pages":[{"name":"Bedroom","buttons":[
  {"id":"b1","label":"Lamp","icon":"power","x":10,"y":10,"w":150,"h":90,
   "entity_id":"light.bedroom_lamp","action":"toggle","color":"#F5A623"}
]}]}
```

Coordinates are pixels in a 480x280 area (the bottom 40 px is the page tab/status bar). Actions: `toggle`, `on`, `off`, `scene`, `script`, `press`. Icons: `power, eye, home, play, pause, bell, settings, ok, wifi, audio, charge, image` (LVGL built-in symbols).

## Layout of the code

| File | Purpose |
|---|---|
| `src/display.h` | LovyanGFX pins for ST7796 + FT6336 *(guess, standard published values)* |
| `src/ui.cpp` | LVGL 9 setup, builds buttons from the layout |
| `src/ha_client.cpp` | HA WebSocket auth, subscribe, service calls |
| `src/web.cpp`, `src/web_pages.h` | HTTP API + editor and settings pages |
| `src/config.cpp` | Settings and layout storage |

## Known gaps and guesses

- All hardware behavior: pins, rotation (`setRotation(1)`; use 3 if upside down), touch axes, backlight, PSRAM setting (`qio_qspi`, 16 MB flash) are *(guess)* and untested.
- `subscribe_entities` message parsing (`a` snapshot and `c` change formats) follows my reading of HA's frontend protocol; it compiles but is untested against a live Home Assistant.
- TLS (`wss`) path is untested and does not verify certificates.
- No authentication on the web editor: anyone on your LAN can edit the layout or settings. Add auth before exposing beyond a trusted network.
- No entity picker yet; entity ids are typed in. Next ideas: fetch the entity list from HA, sliders/dimmers, sensor tiles, screen dimming/sleep, OTA updates.
