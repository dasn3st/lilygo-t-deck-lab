# Current Firmware Source Snapshot

**Date:** 6 October 2026 · **Build label:** `2.8.0.d8db430` · **PlatformIO environment:** `t-deck-tft`

This is the current source snapshot copied from the active T-UI worktree. It includes the firmware/UI changes built and flashed on 6 October 2026, including Notes and Maps QR downloads, map marker image sources/generator, the current microphone/Deepgram implementation, and the Mac UI preview.

The copy contains about 2,520 files / 126 MB. Build caches (`.pio`), generated output and device backups (`outputs`), managed third-party components, local `.env`/`.envrc` files, local MCP configuration, Git/worktree metadata, and Python cache files were excluded. The old `firmware/source/` folder in the project map is preserved as an earlier snapshot; use this folder for the source matching the 6 October build. A rebuild from this source may produce a different Git-derived build label because repository metadata is not included.

Build from this source with PlatformIO: `pio run -e t-deck-tft`. The exact prebuilt app and factory images, with SHA-256 hashes, are in `../build/2026-10-06-qr-map/`.

This snapshot does not contain live Deepgram/Ollama API keys or the SD card contents. Keep device backups and private note/map data out of public site assets.
