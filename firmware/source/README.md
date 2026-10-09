# T-UI — firmware source

**T-UI** is an unofficial, community-made custom firmware for the **LilyGo T-Deck**: a
phone-style launcher (apps grid, lock screen, offline maps with pins, notes, calculator,
games, file manager, Wi-Fi file sharing, a Lua app engine for SD-card apps, and more)
built on top of the open-source [Meshtastic](https://meshtastic.org) mesh-radio firmware.

➡️ **Install it the easy way (web installer):** https://jeeab.github.io/t-ui/

This repository is the complete, buildable source for the firmware that installer ships,
published to comply with the GPL-3.0 license of the Meshtastic project it builds on.

## 📌 First things to know (after installing)

- **The screen locks when it sleeps. The default unlock code is `1234`** — change it in
  *Settings → Lock PIN*.
- **Taps landing in the wrong spot? Press `Alt + C`** on the keyboard to re-run touch
  calibration. Works even on the lock screen.
- **Double-click the trackball** to wake the screen and to go Home from anywhere.
- The first start sits on the Meshtastic logo for **15–20 seconds** — normal, don't unplug.

## What this is based on

- Base: [meshtastic/firmware](https://github.com/meshtastic/firmware), `develop` branch,
  commit `8058caf` ("fix(power): correct charge/power detection preprocessor bugs (#10906)").
- The stock Meshtastic UI library (`meshtastic-device-ui`) is vendored under
  `lib/meshtastic-device-ui/` with extensive T-UI modifications.
- Lua 5.4.8 is vendored under `lib/lua/` (MIT license) for the SD-card app engine.
- The upstream Meshtastic README is preserved as `UPSTREAM_README.md`.

Note: `lib/meshtastic-device-ui/maps/` (upstream demo map tiles, ~67 MB of unmodified
sample data) is omitted from this repo to keep it lean; it is not needed to build.
Map tiles are user data loaded onto the SD card.

## Main modifications (vs stock Meshtastic)

- `lib/meshtastic-device-ui/source/graphics/TFT/TFTView_320x240.cpp` (+ header) — the
  launcher itself: home grid with pages/reorder, settings screen (mesh kill switch,
  lock PIN, brightness, GPS on/off — on = always searching, location-sharing precision
  [Exact/Rough/Off via Meshtastic's per-channel position precision], Wi-Fi, FTP file
  share, screen timeout, one Sound switch driving Meshtastic's `buzzer_mode` for beeps
  AND message alerts), lock/PIN wake gating, unread-message counter on the home bar +
  lock screen, Files 2.0 (copy/paste/trash/folders), standalone Maps app with saved
  named pins, crash diagnostics readout, Alt+C touch recalibration, dynamic SD-card
  app tiles.
- New device-ui app modules: `SnakeGame.cpp`, `StopwatchApp.cpp`, `NotesApp.cpp`,
  `CalculatorApp.cpp`, `LuaApp.cpp`.
- New firmware bridges in `src/`: `TDeckMeshSwitch.cpp` (radio kill switch),
  `TDeckGpsBridge.cpp` / `TDeckGpsControl.cpp` (GPS status + live on/off/interval),
  `TDeckMemInfo.cpp` (heap/PSRAM diagnostics + reset-reason capture), `TDeckBeep.cpp`
  (volume bracket), `TDeckWifi.cpp`, `TDeckFtp.cpp` (on-demand FTP server),
  `TDeckLua.cpp` (sandboxed Lua engine for `/apps/<name>/main.lua` on SD).
- `src/main.cpp` — deferred-service hooks for the mesh and GPS switches.
- `src/AudioThread.h`, `platformio.ini`, `variants/esp32s3/t-deck/platformio.ini` —
  small supporting changes.

## Building

Standard Meshtastic build, T-Deck target:

```
platformio run -e t-deck-tft
```

Flash the resulting `firmware-*.factory.bin` at offset `0x0` (full install) or
`firmware-*.bin` at `0x10000` (update, preserves settings).

## License & trademarks

GPL-3.0, same as Meshtastic — see `LICENSE`. T-UI is **not** created, endorsed, or
supported by the Meshtastic project or LilyGo. "Meshtastic" is a registered trademark
of the Meshtastic project, used here descriptively. Use at your own risk.
