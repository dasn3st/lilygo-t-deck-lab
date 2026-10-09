# T-Deck Mac UI Preview

This local browser preview is for exercising the reconstructed T-Deck interface without connecting to or changing the device.

## Start

From this directory run:

```sh
python3 -m http.server 8765 --bind 127.0.0.1
```

Then open <http://127.0.0.1:8765/>. Stop the server with `Ctrl-C` in its terminal.

## Try it

- Click launcher apps or the quick links beside the device.
- The launcher mirrors the recovered source's 15 built-in entries. Twelve fit on the first page; use the trackball arrows to reach the second. The two slim text shortcuts at the bottom open the persistent To-do list and drop a map point.
- To-do entries are stored in this browser's local demo profile. The Point shortcut adds a marker at the map centre in the simulator; firmware uses the live GPS position or, without a fix, the last/home map centre and saves the pin through Maps' existing pin store.
- Mesh, Notes, Ollama, Calendar, Files, Maps, Calculator, Clock, and Terminal use the supplied launcher logo assets. Mesh, Nodes, Get Apps, Settings, Favorites, and Alerts use the inverted monochrome treatment. Maps stays in the launcher on page one.
- In Ollama, open the top-right menu. Tap the model or mode repeatedly; the menu remains open until **Schließen**. `/markdown`, `/save`, and `/kalender` are demo shortcuts.
- T9 is active by default, uses a compact offline word list, and shows up to three prefix suggestions for focused text fields. Tap a suggestion to replace the current word. Turn it on/off in Settings.
- Notes can be created and edited; edits persist in this browser profile when leaving the editor. Star a note to show it in Favorites. Ollama's **Save → Notes** creates a local demo Markdown note.
- Select a calendar day to scroll to its detail editor. The demo supports a day mark, a note, an alarm toggle/time, and a saved event. Alarm controls are display-only and do not ring.
- Terminal follows the native `TerminalApp.cpp` geometry: SSH target and menu in the compact top row, 316×154 output panel at y=48, and command input at y=206. The menu keeps Connect, Read SD, Clear, and three-step output zoom together. SSH and the SD log remain browser-only mocks; leaving/reopening the screen retains the simulated state.
- In Terminal, use the menu's SSH mock or enter `help`, `pwd`, `ls`, `date`, or `meshtastic status`.
- The map uses four real local OSM-Carto PNG tiles at zoom 16 from the prepared Berlin map source. Changing zoom changes the demo indicator only; changing Tram/S-Bahn/U-Bahn/WLAN only demonstrates the selector, not rendered overlays.
- Open **ÖPNV** from the Maps screen (or Maps menu) for the five supplied offline network plans: S-/U-Bahn, tram, bus, regional rail, and night network. Their A3 pages are rasterized at 3200 px wide and stored locally as high-quality WebP; use the menu to switch plans, zoom, recenter, or return to OSM. Drag the image to pan when zoomed. These are static PDF snapshots, not live routing, timetables, or real-time service information.
- The keyboard shown on the device accepts clicks while an input is active. The preview also accepts the Mac keyboard.

## Abnahme (Mac-Simulator)

Im Browser gegengeprüft: alle Launcher-Logos laden; Ollama-Modell/Modus lassen sich im offenen Menü wechseln; Chat → Markdown → Save landet in Notes und kann favorisiert werden; Kalendernotiz bleibt beim Verlassen erhalten; T9-Vorschläge lassen sich antippen; Terminalbefehle und Verlauf bleiben beim Wiederöffnen erhalten; Rechnerbeispiel `7 + 5 = 12`; alle fünf ÖPNV-Bilder laden, Planwechsel, Zoom, Verschieben und Rückkehr zu OSM funktionieren. Der Lauf hatte keine JavaScript-Fehler und keine fehlgeschlagenen lokalen Asset-Requests.

## Scope and safety

All application data is stored in this browser's local storage. The preview makes no cloud/API calls and does not contact the T-Deck. Meshtastic radio, GPS, real SSH, microphone, Ollama inference, and SD-card behavior are not emulated. The transit maps are simulator assets only; they have not been added to the device SD card or firmware. This is an interaction/design simulator, not an ESP32 firmware emulator or a replacement for the PlatformIO firmware build.

Use **Demo zurücksetzen** to clear only this preview's saved demo data.
