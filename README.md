# T-Deck Lab

An extended LilyGO T-Deck firmware project: Meshtastic, Notes, shared Deepgram voice input, maps and the separate MeshGame exploration layer.

This repository is a cleaned release snapshot of the source and the matching application image flashed on 9 October 2026. It keeps the upstream Meshtastic GPL license with the firmware source. Large offline map collections, personal backups, SD-card data, local credentials and machine-specific settings are not included.

## Start here

- [Project story and website-ready facts](docs/PROJEKTSTORY-WEBSITE.md)
- [Microphone, ES7210, I²S and Deepgram implementation](docs/AUDIO-DEEPGRAM-IMPLEMENTATION.md)
- [MeshGame quest system, candidate catalogue and current limits](docs/MESHGAME-QUESTMASTER.md)
- [Firmware source](firmware/source/)
- [Flashed application image and checksum](firmware/build-2026-10-09/README.md)

## Firmware image

- Board: LilyGO T-Deck / ESP32-S3
- PlatformIO target: `t-deck-tft`
- Firmware app version: `2.8.0`
- Flash offset: `0x10000` (application slot only)
- SHA-256: `77d4c02c39075a188dbf7dc476b4a7806bb46c59a58da90e3fff5ca5237714d2`

The 9 October image contains MeshGame's Berlin candidate catalogue, quest help/labels and the shared microphone/Deepgram source. The microphone had previously produced a user-confirmed clean Notes transcription, but was not re-tested against Deepgram after this particular flash. See the audio notes for the exact evidence and limits.

## Build

Install PlatformIO and the project's dependencies, then from `firmware/source/` run:

```sh
pio run -e t-deck-tft
```

Review the included upstream license and project-specific configuration before building or flashing. Ordinary app updates should write only the application slot and preserve NVS, partitions, and SD-card contents.

## Important boundaries

- Deepgram is an online prerecorded-audio transcription service; it is independent of Ollama.
- The Deepgram API key belongs in `/deepgram.txt` on the device SD card. Only placeholder examples belong in this repository.
- Recordings are mono 16 kHz/16-bit WAV, buffered in PSRAM, with a 30-second maximum that can be shorter under memory pressure.
- MeshGame candidate entries are unverified leads, not proof that a Wi-Fi network exists at the location.

## Release hygiene

This repository intentionally excludes offline map archives, device/NVS backups, actual `/deepgram.txt` or `/ollama.txt` files, Wi-Fi credentials, private notes, and user-specific device logs. Do not add those files to a public commit.
