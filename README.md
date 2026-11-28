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

- **Web:** `http://bedside.local/update` (linked from the layout editor and Settings pages). Either **Check for a new version** and **Install** the latest GitHub release (live progress, the same as the touchscreen), or choose a `-ota.bin` file and upload it. The device reboots.
- **Touchscreen:** gear, **Check update** (asks GitHub for the latest release), then **Install** downloads that release's `-ota.bin` and flashes it.
- **Progress:** every install (web upload or GitHub download) shows a full-screen card on the device with the phase (looking up, downloading/writing with a percent bar, verifying, rebooting) and any error with a Close button. The `/update` page shows upload percent, verifying, installed, then waits for the device to come back and shows the new version. Use the `-ota.bin`, not `-full.bin`.

Caveats: the on-device GitHub update does not verify TLS certificates and images are not signed, and it only works if the repository is public. All OTA paths are *untested on hardware*; keep USB access handy.

## Controls and Home Assistant addresses

- **Buttons:** toggle, on, off, scene, script, press, cover open/close/stop, lock/unlock, media play/pause. Buttons light up when the entity is on/open/playing/unlocked/heating.
- **Sliders** (add with "+ Add slider" in the editor): light brightness, light color temperature (Kelvin), cover position, fan speed, media volume, climate target temperature, `number` and `input_number`. Sliders follow the entity's live value and send while you drag (at most every 300 ms) and on release. Optional `min`/`max` override the defaults. RGB color wheels and HVAC mode pickers are not done yet.
- **Slider inside a button:** in the editor, a button's settings have a checkbox *Also show a slider for this entity*. The slider is part of the button (`"with_slider":true`, optional `"slider_attr"`): the button grows 40 px taller and the slider sits in its bottom 34 px; unticking shrinks it back. Older layouts that have a separate slider with `linked` set to a button are merged into that button automatically when loaded (on the device and in the editor; the editor saves the merged form on your next save).
- **Entity picker:** the editor loads your devices from Home Assistant (lights, switches, covers, fans, climate, media players, scenes, scripts, helpers, locks, buttons) so you can pick them by name; typing an id still works. It needs the Home Assistant connection to be up.
- **Local and external URL:** Settings has a local address (tried first) and an optional external address, for example a Nabu Casa or reverse-proxy name. If the local one does not authenticate within 12 s the device switches to the external one, and while on external it retries local every 5 minutes.

## Touchscreen settings menu and status icons

- **Gear (bottom right)** opens the settings menu with **Wi-Fi** and **About**.
  - **Wi-Fi:** shows the current network and IP, plus the saved-network list. Tap a saved network to change its password, move it up/down in priority, join it now, or delete it. **Scan** lists nearby networks (tap one, type the password). **Add network** lets you type an SSID and password by hand for a network that is not in range. Save stores without connecting; Connect joins, and on success saves it as top priority and restarts.
  - **About:** firmware version, IP, web address, web login, Home Assistant status, and **Check update** / **Install**.
- **Status icons** replace the old status text: a Wi-Fi icon and an outlined **HA** badge (shows `HA ext` when connected through the external address). Green is connected, amber is connecting or weak signal, red is a rejected token, gray is not set up. Tap the Wi-Fi icon to open the Wi-Fi settings. Style borrowed from the SpoolmanScale display (outlined badge, mint green on dark). The icons are small and sit just left of the gear.
- *All of this is untested on the board; the keyboard layout in particular may need tuning.*

## Reboot and factory reset

At the bottom of the web Settings page: **Reboot device** restarts and keeps everything. **Factory reset** erases saved Wi-Fi networks, the HA token and addresses, the web password and both layouts, then restarts into setup mode. It asks twice: a confirm dialog, then you must type `RESET`. A new web password is generated and shown on the screen (gear, About).

## Screen rotation

Gear, **Display** (or `Settings` on the web) rotates the screen: 0 (default landscape), 90 clockwise, 180, or 90 counter-clockwise. The device restarts to apply it and the touch panel follows. Landscape (0 and 180) uses the normal layout. Portrait (90 and 270) is 320 x 480 and has its **own layout** (the editor has a Landscape/Portrait switch, canvas 320 x 440). Until you save a portrait layout, the landscape one is squeezed to 2/3 of its width. *Untested on hardware: if 90 and 270 come out swapped, change `1 + cfg.rotation` to `1 - cfg.rotation` in `uiInit` (`src/ui.cpp`). The settings screens in portrait are compressed to fit and may look cramped.* If a rotation leaves the touch panel unusable, set it back at `http://bedside.local/settings`. The layout editor also has a rotate control; its Landscape/Portrait selector only chooses which layout you are editing and does not turn the screen. The boot log prints the rotation setting and panel size.

## Camera button

Add a **camera button** in the layout editor (`type: "camera"`). Give it either a Home Assistant camera entity (`camera.front_door`, streamed through HA's camera proxy with your long-lived token, local or external address) or an IP camera URL (`http://user:pass@192.168.1.50/mjpeg`, basic auth only). Tapping the button opens the stream full screen; a tap or swipe closes it.

- **Full brightness option:** tick *Force full screen brightness while the camera is open* in the camera button's settings in the web editor (`"full_brightness": true`). The backlight goes to 100 % (ignoring the brightness maximum) while the view is open and returns to your saved brightness when it closes. There is no on-screen button editor, so this is set in the web editor only.
- Formats: MJPEG (`multipart/x-mixed-replace`) or JPEG snapshots (polled repeatedly). RTSP and H.264 are not supported and cannot be on this chip. Progressive JPEGs are not supported.
- Home Assistant entity: it tries `/api/camera_proxy_stream/<entity>` first and falls back to snapshots (`/api/camera_proxy/<entity>`).
- Expected frame rate (an estimate, *not measured*): a native MJPEG camera at 640x480 may reach roughly 8 to 15 fps; larger frames or weak Wi-Fi are slower. Cameras that HA turns into a stream itself (RTSP, ONVIF and most generic cameras) are usually limited by HA to about 1 to 2 fps. Snapshot polling is about 2 to 5 fps. This is not full-motion video for every camera: for the best result use a camera's own MJPEG URL at 640x480 or lower.
- Frames are shown at 1, 1/2, 1/4 or 1/8 size so they fit the screen without slow resampling (a 640x480 picture shows at 320x240, centered). The overlay shows the live fps.
- TLS certificates are not verified for camera and HA video requests.

## Wi-Fi networks

- Multiple networks are saved in priority order (index 0 first). At boot the device scans and joins the highest-priority saved network that is visible, then tries hidden ones. If none join it opens the `Bedside-Setup` AP and keeps rescanning every 60 s, restarting when a saved network appears. If connected-mode Wi-Fi is lost for over 2 minutes it restarts.
- The same management is on the web settings page (Wi-Fi networks section): scan, add (including out-of-range networks), edit password, reorder, delete, use now.
- Saved passwords are never returned by the API (only a `has_pass` flag); a blank password on edit keeps the old one. Old single `wifi_ssid/wifi_pass` configs migrate automatically.
- *Untested on hardware.* Scans block briefly, so the UI may pause for a moment.

## Touchscreen gestures and brightness

- **Swipe down from the top edge** to open the brightness panel (like an Android quick panel). Drag the slider; the change is applied live and saved when you let go. The panel closes after 6 s idle, when you tap outside it, or when you swipe up.
- The slider only covers the **minimum and maximum brightness** set on the Settings page (limits: minimum 5 to 90 %, maximum at least 10 points above the minimum and up to 100 %), so the screen can never be turned fully dark.
- **Swipe left or right** to change page. Swipes that start on a slider are ignored, and a swipe never triggers the button it ends on. *All gesture thresholds are guesses, untested on the real touch panel.*

## Layout editor sizes

Sizes and positions are shown and editable in **grid cells** (1 cell = 20 px, so the usable area is 24 x 14 cells): width, height, column and row, next to drag and resize, which snap to a quarter of a cell (5 px). `layout.json` still stores pixels, so existing layouts keep working; a button that is not a whole number of cells shows a fraction such as 7.5 x 4.5.

The editor preview draws each button with its icon and the device text sizes (icon 24 px, label 18 px, slider caption 14 px, 12 px slider track with knob, all scaled with the preview), so it looks like the screen apart from the font (the device uses Montserrat, the preview uses it only if installed) and the icon shapes (look-alikes of the built-in LVGL symbols). It is the device screen area scaled to fit the browser: one grid cell is always the same size for the grid and every element, at any browser width, and elements are clipped to (and on load moved inside) the screen area. Device and editor both use the same pixel coordinates (origin at the top left of the 480 x 280 canvas).

Button labels shrink to fit: the device (and the preview) uses the largest of 18, 16, 14 or 12 px that fits the button width on one line; if 12 px is still too long the text wraps to two lines (one line on buttons under 60 px tall) and ends in `...`. Sliders have a 10 px track and a 20 px knob.

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
| `src/camera.cpp` | Full-screen camera view: MJPEG/snapshot download task and JPEG decode *(untested)* |
| `src/improv.cpp` | Improv Wi-Fi serial provisioning *(untested)* |
| `scripts/` | version injection, release-image merge, version bump, flasher manifest |
| `site/` | web flasher page (ESP Web Tools) |

## Known gaps and guesses

- All hardware behavior: pins, rotation (`setRotation(1)`; use 3 if upside down), touch axes, backlight, PSRAM setting (`qio_qspi`, 16 MB flash) are *(guess)* and untested.
- `subscribe_entities` message parsing (`a` snapshot and `c` change formats) follows my reading of HA's frontend protocol; it compiles but is untested against a live Home Assistant.
- TLS (`wss`) path is untested and does not verify certificates.
- No authentication on the web editor: anyone on your LAN can edit the layout or settings. Add auth before exposing beyond a trusted network.
- No entity picker yet; entity ids are typed in. Next ideas: fetch the entity list from HA, sliders/dimmers, sensor tiles, screen dimming/sleep, OTA updates.
