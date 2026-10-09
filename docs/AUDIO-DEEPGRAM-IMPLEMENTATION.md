# T-Deck microphone and Deepgram voice input

Verified against the active source snapshot `firmware/source-current-2026-10-06/` and project documentation on 9 October 2026. This is the project-specific implementation record for future firmware and website work. Recheck source and the physical device before presenting it as current.

## What the feature does

The shared T-UI voice service records speech locally after the user starts recording, stops on the second mic-button press, wraps the captured audio as WAV, and sends it over the device's Wi-Fi/Internet connection to Deepgram's prerecorded transcription endpoint. The returned German transcript is inserted into the text field that had focus when recording began. It is not an offline recognizer, does not use Ollama, and does not stream live audio.

The mic control can be enabled in Notes, the Ollama Agent and Terminal; the same recording/transcription service is reused. Voice activation is a user setting. A mic indicator and recording countdown/status are presented while recording. Code state is in `src/TDeckVoice.cpp` and `src/TDeckVoice.h`; app/menu integration and SD config parsing are in `src/TDeckGemini.cpp` and related T-UI modules.

## Hardware and audio path

- Board: LilyGO T-Deck, ESP32-S3, two onboard microphone inputs through the ES7210 codec.
- Codec is probed and initialized over I²C. The source configures both inputs (`AUDIO_HAL_ADC_INPUT_ALL`, `MIC1 | MIC2`), normal I²S framing, codec slave mode, 16 kHz, 16-bit samples, configured gains per mic and ADC start.
- T-Deck pins come from `variants/esp32s3/t-deck/variant.h`: MCLK GPIO48, BCLK/SCK GPIO47, data-in GPIO14, LRCK/WS GPIO21. The driver listens on I²S1 (`I2S_NUM_1`); the existing speaker path uses I²S0.
- I²S receives stereo left/right 16-bit slots. The default automatic mode compares channel level for each DMA block and puts the louder slot into the mono recording. User-selectable modes can force MIC1 or MIC2. The two inputs are not averaged together because phase differences can cancel speech.
- Output is mono, 16 kHz, signed 16-bit PCM in a standard 44-byte WAV header. Audio is buffered in PSRAM rather than written to the SD card. A 30-second mono take is about 960,000 bytes of PCM, plus the header.
- The code caps a take at 30 seconds. It computes capacity from the largest free PSRAM block, reserves memory for the UI/services and reduces the take if necessary; it fails below a 2-second minimum. It is therefore not unlimited and may stop sooner under memory pressure.
- The important repeat-capture behavior: after setup, the ES7210 remains initialized between recordings; the RX I²S driver is stopped/uninstalled after capture. The code comments document that repeatedly stopping/resetting the codec led to silent later takes on this device. This is the implemented mitigation, not a claim that every codec failure is impossible.

## SD-card configuration and secret handling

Deepgram reads `/deepgram.txt` from the T-Deck SD card. It accepts `key=`, `stt_key=`, `deepgram_key=` or `api_key=` for the credential. Optional settings accept `url=`/`stt_url=`, `model=`/`stt_model=` and `language=`/`stt_language=`. Defaults are `https://api.deepgram.com/v1/listen`, `nova-3`, and `de`.

Safe documentation example (the value below is deliberately fake):

```ini
key=YOUR_DEEPGRAM_API_KEY
model=nova-3
language=de
```

Never copy the real key, a real `/deepgram.txt`, Wi-Fi credentials, or SD-card contents into source control, logs, a screenshot, documentation, or website. Only publish the field names and placeholder example.

Ollama is separate. Its agent/chat configuration is read from `/ollama.txt` and its requests use the Ollama flow. Deepgram's key must not be placed in `/ollama.txt`; no key is compiled into the firmware. Keep the two help/config entries distinct in the UI and on the website.

## Request/response protocol

1. The user stops the recording. Firmware finalizes the mono WAV in PSRAM.
2. For Deepgram, the WAV bytes (including WAV header) are the raw HTTP request body with `Content-Type: audio/wav`; this differs from the multipart request used by the alternate OpenAI-compatible path.
3. The request URL adds `model=nova-3`, `language=de`, `smart_format=true`, and `punctuate=true` by default (the values can come from the config file).
4. Authorization is `Authorization: Token <key>` for Deepgram. The request is queued to the shared T-Deck network client over HTTPS.
5. The JSON parser reads `results.channels[0].alternatives[0].transcript`. A non-empty transcript is delivered to the previously focused LVGL text area at its insertion point.

Useful source pointers:

- `src/TDeckVoice.cpp`: codec setup, I²S RX, channel selection, WAV header, buffer sizing, Deepgram upload, auth, response handling and recording state.
- `src/TDeckVoice.h`: public voice service interface.
- `src/TDeckGemini.cpp`: `/deepgram.txt` parsing and separate Ollama/Deepgram configuration.
- `variants/esp32s3/t-deck/variant.h`: board audio pin definitions.
- `lib/es7210/`: ES7210 driver bundled in this source snapshot.
- `docs/PROJEKTDOKUMENTATION.md`: broader project architecture and handoff evidence.

## Firmware and device verification boundary

The latest documented app image is `firmware-t-deck-tft-2.8.0.bin`, PlatformIO target `t-deck-tft`, flashed app-only at `0x10000` on 9 October 2026. SHA-256: `77d4c02c39075a188dbf7dc476b4a7806bb46c59a58da90e3fff5ca5237714d2`. See `firmware/build/2026-10-09-meshgame-help-labels-refined/README.md` in the project for the flash record. The image contains the microphone code from the active source snapshot.

The user confirms that transcription works generally in every integrated text-entry app: Notes, the Ollama Agent and Terminal. They previously described a clean Notes transcription. Treat cross-app functionality as user-confirmed behavior on the current device. Keep the evidence source explicit: this is the user's device confirmation, not an independent test performed by Codex after the 9 October 2026 flash. The source/build/flash evidence and user-reported end-to-end behavior are separate.

## Troubleshooting history and the actual success point

This sequence is reconstructed from the user's reports in the project conversation and checked against the current implementation where possible. It is useful project history, but it is not a timestamped lab log; do not invent exact dates or serial measurements.

1. **Separate playback from recording.** The speaker ping worked, but that only verified speaker output. Early mic-level attempts showed no input or distorted audio, and later the two ES7210 channels were inspected separately. The user then confirmed that MIC1 produced a visible level. Some diagnostic retries coincided with the SD card disappearing; this was an observed device symptom, not proof that every failed mic attempt was caused by SD storage. The voice capture itself buffers in PSRAM.
2. **Correct the service boundary.** An early API-help/config mix-up was corrected: `/ollama.txt` is for Ollama agent/chat, while `/deepgram.txt` holds the separate speech-to-text endpoint and key. Keep this separation in UI, docs and web copy.
3. **First successful transcription.** The user explicitly reported that transcription worked and then confirmed it was very clean, even preserving improvised/unclear speech. This is the key success checkpoint: the device captured speech, Deepgram returned text, and the text reached Notes. Later attempts were intermittent, so the first success alone did not prove repeatability.
4. **Repeat-capture failure and code response.** The user reported that level tests and transcription could work once and then fail. The source now probes/configures the ES7210 once and keeps it initialized across takes, while stopping and uninstalling the I²S1 RX driver after a capture. The source comments state repeated codec STOP/reset cycles had caused silent later captures on this device. This lifecycle change is the code-level mitigation. Do not claim an independently measured root cause for the SD-removal symptom, or claim that the mitigation prevents every failure.
5. **Usable capture duration.** The user asked for longer speech because the earlier short takes felt rushed. A visible remaining-time countdown and PSRAM-sized buffer were implemented, with a maximum of 30 seconds and a possibly shorter limit when memory is fragmented/low. The current source confirms this limit; it is not unlimited recording.
6. **Transient ES7210 setup message.** The user later reported seeing `ES7210 microphone setup failed` once, then said it worked on a subsequent attempt. That report does not establish why the one-off setup failed. Preserve this as a known intermittent observation instead of writing that it was fully resolved.
7. **Cross-app confirmation.** The user confirms that voice transcription works in every integrated text-input app, including Notes, Ollama Agent and Terminal. Record this as current user-confirmed behavior. Codex did not independently exercise each app after the latest flash.

For future acceptance notes, record separately: (a) codec detected/setup success, (b) MIC1/MIC2 level responds while speaking, (c) a WAV is captured, (d) one transcription returns, (e) a second consecutive transcription returns without reboot, and (f) the returned text is inserted in each enabled app. Current cross-app text insertion is user-confirmed. A speaker beep alone only verifies playback.

## Website-ready facts and careful claims

Safe concise description: “The T-Deck records mono 16 kHz/16-bit WAV through its ES7210 microphone codec. After the user stops recording, the firmware sends the clip over Wi-Fi to Deepgram's prerecorded speech-to-text API; the transcript returns to the active text field. Deepgram credentials live in a separate SD-card config file, apart from Ollama.”

Explain the engineering challenge accurately: the speaker ping only confirmed speaker output, not microphone input. The useful debugging split was codec detection/configuration, left/right I²S input and levels, repeat-capture lifecycle, memory capacity, WAV formatting, network/auth and transcript insertion. The T-Deck's speaker uses I²S0 while this microphone path uses I²S1.

Avoid unverified claims such as “first T-Deck with speech recognition,” “unlimited recording,” “offline transcription,” or “streaming transcription.” It is accurate to say the user confirms transcription works in Notes, Ollama Agent and Terminal. Do not imply Codex independently retested after the latest flash. Do not publish the API key or private config contents.

## Official references

- [LilyGO T-Deck hardware documentation](https://wiki.lilygo.cc/products/t-deck-series/t-deck/)
- [LilyGO T-Deck audio/microphone UnitTest source](https://github.com/Xinyuan-LilyGO/T-Deck/blob/master/examples/UnitTest/UnitTest.ino)
- [Deepgram prerecorded speech-to-text API](https://developers.deepgram.com/reference/speech-to-text/listen-pre-recorded?explorer=true)
- [Deepgram audio encoding guidance](https://developers.deepgram.com/docs/encoding/)
