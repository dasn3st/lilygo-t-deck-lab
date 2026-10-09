// T-Deck voice input: microphone recording -> Deepgram prerecorded transcription API.
// Ollama chat credentials and requests are handled separately by TDeckGemini.cpp.
#include "TDeckVoice.h"

#include "TDeckGemini.h"
#include "main.h" // main-loop bridge to the existing I2S speaker output
#include "AudioThread.h"
#include "configuration.h"
#include <es7210.h>
#if defined(HAS_SDCARD)
#endif
#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" bool tdeck_net_post_bytes(const char *url, const uint8_t *body, size_t bodyLen,
                                      const char *contentType);
extern "C" bool tdeck_net_post_bytes_owned(const char *url, uint8_t *body, size_t bodyLen,
                                            const char *contentType);
extern "C" void tdeck_net_set_auth(const char *value);
extern "C" int tdeck_net_poll(void);
extern "C" int tdeck_net_result(char *buf, int cap);
extern "C" void tdeck_net_reset(void);
extern "C" int tdeck_net_http_code(void);

volatile bool tdeck_voice_request = false;
static volatile uint8_t gInputLevel = 0;
static volatile uint8_t gInputLevelMic1 = 0;
static volatile uint8_t gInputLevelMic2 = 0;

namespace tdeckvoice
{
namespace
{
constexpr int kSampleRate = 16000;
// Buffer up to 30 seconds of mono 16 kHz / 16-bit audio in PSRAM. Deepgram uploads after stop;
// the network queue takes ownership of this buffer so a second full-sized copy is not needed.
constexpr int kMaxRecordSeconds = 30;
constexpr int kMaxDiagnosticSeconds = 10;
constexpr size_t kRecordAudioBytes = (size_t)kSampleRate * 2 * kMaxRecordSeconds;
constexpr size_t kDiagnosticAudioBytes = (size_t)kSampleRate * 2 * kMaxDiagnosticSeconds;
// Leave some PSRAM free for the UI and concurrent services. A fixed two-minute
// allocation failed on devices whose largest free PSRAM block was smaller than 3.8 MB.
constexpr size_t kRecordPsramReserveBytes = 256 * 1024;
constexpr size_t kMinimumCaptureBytes = (size_t)kSampleRate * 2 * 2; // fail only below 2 seconds
constexpr size_t kWavHeader = 44;
constexpr int kResultCap = 2048;
constexpr i2s_port_t kI2sPort = I2S_NUM_1; // LilyGO UnitTest: mic=I2S1, speaker=I2S0

volatile State gState = IDLE;
volatile bool gStopRequested = false;
volatile bool gDiagnostic = false;
char gStatus[96] = {0};
char gTranscript[700] = {0};
TaskHandle_t gTask = nullptr;
uint8_t *gAudio = nullptr;
size_t gCaptureCapacityBytes = 0;
volatile uint32_t gCaptureStartedMs = 0;
uint8_t *gDiagnosticAudio = nullptr;
size_t gDiagnosticAudioLen = 0;
volatile bool gPlaybackPending = false;
bool gPlaybackStarted = false;
uint8_t *gUpload = nullptr;
size_t gUploadLen = 0;
volatile bool gUploadReady = false;
volatile uint8_t gMicGainIndex[2] = {2, 2}; // ES7210 GAIN_6DB per microphone
volatile uint8_t gMicInputMode = 0; // 0=automatic, 1=MIC1, 2=MIC2
volatile uint8_t gSpeakerVolume = 20; // existing gentle default, in percent
volatile bool gSpeakerGainApplyPending = false;
bool gAudioSettingsLoaded = false;
bool gVoiceEnabled = false;
bool gCodecInitialized = false;
volatile bool gMicGainSavePending = false;
volatile bool gMicInputModeSavePending = false;
volatile bool gSpeakerVolumeSavePending = false;
char gUrl[256] = {0};
char gKey[96] = {0};
char gModel[64] = {0};
char gLanguage[16] = {0};
bool gApplyPending = false;
lv_obj_t *gTarget = nullptr;
uint32_t gUploadStartedMs = 0;
constexpr uint8_t kMicGainCount = 15;
const es7210_gain_value_t kMicGains[kMicGainCount] = {
    GAIN_0DB, GAIN_3DB, GAIN_6DB, GAIN_9DB, GAIN_12DB, GAIN_15DB, GAIN_18DB, GAIN_21DB,
    GAIN_24DB, GAIN_27DB, GAIN_30DB, GAIN_33DB, GAIN_34_5DB, GAIN_36DB, GAIN_37_5DB,
};

uint8_t clampLevel(uint32_t average)
{
    return average >= 1800 ? 100 : (uint8_t)(average * 100 / 1800);
}

void loadAudioSettings()
{
    if (gAudioSettingsLoaded)
        return;
    gAudioSettingsLoaded = true;
    Preferences prefs;
    if (prefs.begin("tdeckaudio", true)) {
        gVoiceEnabled = prefs.getBool("voice_enabled", false);
        gMicGainIndex[0] = (uint8_t)prefs.getUChar("mic1_gain", 2);
        gMicGainIndex[1] = (uint8_t)prefs.getUChar("mic2_gain", 2);
        gMicInputMode = (uint8_t)prefs.getUChar("mic_input", 0);
        gSpeakerVolume = (uint8_t)prefs.getUChar("speaker", 20);
        prefs.end();
    }
    if (gMicGainIndex[0] >= kMicGainCount)
        gMicGainIndex[0] = 2;
    if (gMicGainIndex[1] >= kMicGainCount)
        gMicGainIndex[1] = 2;
    if (gMicInputMode > 2)
        gMicInputMode = 0;
    if (gSpeakerVolume > 100)
        gSpeakerVolume = 20;
    gSpeakerGainApplyPending = true;
}

void setStatus(const char *text)
{
    snprintf(gStatus, sizeof(gStatus), "%s", text ? text : "");
}

void writeLe16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

void writeLe32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void writeWavHeader(uint8_t *p, size_t pcmBytes)
{
    memcpy(p, "RIFF", 4);
    writeLe32(p + 4, (uint32_t)(36 + pcmBytes));
    memcpy(p + 8, "WAVEfmt ", 8);
    writeLe32(p + 16, 16);
    writeLe16(p + 20, 1); // PCM
    writeLe16(p + 22, 1); // mono
    writeLe32(p + 24, kSampleRate);
    writeLe32(p + 28, kSampleRate * 2);
    writeLe16(p + 32, 2);
    writeLe16(p + 34, 16);
    memcpy(p + 36, "data", 4);
    writeLe32(p + 40, (uint32_t)pcmBytes);
}

bool initCodec()
{
    // LilyGO's own T-Deck example initializes the ES7210 once and leaves it running.
    // Repeated STOP/reset cycles made later captures report silence on this device.
    if (gCodecInitialized)
        return true;

    // Configure both physical mic inputs on the ES7210. T-Deck has one I2S data-in
    // wire, so the codec sends the two ADC channels as ordinary left/right I2S.
    Wire.beginTransmission(ES7210_ADDR);
    if (Wire.endTransmission() != 0) {
        LOG_ERROR("[VOICE] ES7210 not found at 0x%02x", ES7210_ADDR);
        return false;
    }

    audio_hal_codec_config_t cfg = {
        .adc_input = AUDIO_HAL_ADC_INPUT_ALL,
        .codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE,
        .i2s_iface = {
            .mode = AUDIO_HAL_MODE_SLAVE,
            .fmt = AUDIO_HAL_I2S_NORMAL,
            .samples = AUDIO_HAL_16K_SAMPLES,
            .bits = AUDIO_HAL_BIT_LENGTH_16BITS,
        },
    };
    esp_err_t err = es7210_adc_init(&Wire, &cfg);
    if (err == ESP_OK)
        err = es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface);
    if (err == ESP_OK)
        err = es7210_mic_select((es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2));
    if (err == ESP_OK)
        err = es7210_adc_set_gain(ES7210_INPUT_MIC1, kMicGains[gMicGainIndex[0]]);
    if (err == ESP_OK)
        err = es7210_adc_set_gain(ES7210_INPUT_MIC2, kMicGains[gMicGainIndex[1]]);
    if (err == ESP_OK)
        err = es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);
    if (err != ESP_OK)
        LOG_ERROR("[VOICE] LilyGO ES7210 initialization failed: %s (%d)", esp_err_to_name(err), (int)err);
    else
        LOG_INFO("[VOICE] ES7210 regs: clock=%02x power=%02x fmt=%02x route=%02x mic1_gain=%02x mic2_gain=%02x",
                 es7210_read_reg(ES7210_CLOCK_OFF_REG01), es7210_read_reg(ES7210_POWER_DOWN_REG06),
                 es7210_read_reg(ES7210_SDP_INTERFACE1_REG11), es7210_read_reg(ES7210_SDP_INTERFACE2_REG12),
                 es7210_read_reg(ES7210_MIC1_GAIN_REG43), es7210_read_reg(ES7210_MIC2_GAIN_REG44));
    gCodecInitialized = err == ESP_OK;
    return gCodecInitialized;
}

bool initI2s()
{
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate = kSampleRate;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    // ES7210 is configured for normal I2S, which is a two-slot left/right frame.
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 64;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    cfg.fixed_mclk = 0;
    cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    cfg.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;
    esp_err_t err = i2s_driver_install(kI2sPort, &cfg, 0, nullptr);
    if (err != ESP_OK) {
        LOG_ERROR("[VOICE] I2S install failed: %s (%d)", esp_err_to_name(err), (int)err);
        return false;
    }

    i2s_pin_config_t pins = {};
    pins.mck_io_num = ES7210_MCLK;
    pins.bck_io_num = ES7210_SCK;
    pins.ws_io_num = ES7210_LRCK;
    pins.data_in_num = ES7210_DIN;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    err = i2s_set_pin(kI2sPort, &pins);
    if (err != ESP_OK) {
        LOG_ERROR("[VOICE] I2S pin setup failed: %s (%d)", esp_err_to_name(err), (int)err);
        i2s_driver_uninstall(kI2sPort);
        return false;
    }
    err = i2s_zero_dma_buffer(kI2sPort);
    if (err == ESP_OK)
        err = i2s_start(kI2sPort);
    if (err != ESP_OK) {
        LOG_ERROR("[VOICE] I2S start failed: %s (%d)", esp_err_to_name(err), (int)err);
        i2s_driver_uninstall(kI2sPort);
        return false;
    }
    return true;
}

void stopI2s()
{
    const esp_err_t stopErr = i2s_stop(kI2sPort);
    const esp_err_t uninstallErr = i2s_driver_uninstall(kI2sPort);
    if (stopErr != ESP_OK && stopErr != ESP_ERR_INVALID_STATE)
        LOG_WARN("[VOICE] I2S stop: %s (%d)", esp_err_to_name(stopErr), (int)stopErr);
    if (uninstallErr != ESP_OK)
        LOG_WARN("[VOICE] I2S uninstall: %s (%d)", esp_err_to_name(uninstallErr), (int)uninstallErr);
}

void stopCodec()
{
    const esp_err_t err = es7210_adc_ctrl_state(AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_STOP);
    gCodecInitialized = false;
    if (err != ESP_OK)
        LOG_WARN("[VOICE] ES7210 stop failed: %s (%d)", esp_err_to_name(err), (int)err);
}

void makeMultipart(size_t pcmBytes)
{
    static const char boundary[] = "----TDeckVoiceBoundary7MA4YWxkTrZu0gW";
    char prefix[420];
    const int prefixLen = snprintf(prefix, sizeof(prefix),
                                   "--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n"
                                   "--%s\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\n%s\r\n"
                                   "--%s\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n"
                                   "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"tdeck.wav\"\r\n"
                                   "Content-Type: audio/wav\r\n\r\n",
                                   boundary, gModel, boundary, gLanguage, boundary, boundary);
    if (prefixLen <= 0 || (size_t)prefixLen >= sizeof(prefix))
        return;
    const char suffix[] = "\r\n--" "----TDeckVoiceBoundary7MA4YWxkTrZu0gW" "--\r\n";
    const size_t total = (size_t)prefixLen + kWavHeader + pcmBytes + sizeof(suffix) - 1;
    uint8_t *body = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!body) {
        setStatus("Not enough memory for audio upload");
        gState = FAILED;
        return;
    }
    memcpy(body, prefix, (size_t)prefixLen);
    memcpy(body + prefixLen, gAudio, kWavHeader + pcmBytes);
    memcpy(body + prefixLen + kWavHeader + pcmBytes, suffix, sizeof(suffix) - 1);
    gUpload = body;
    gUploadLen = total;
    gUploadReady = true;
}

bool isDeepgramEndpoint()
{
    return strstr(gUrl, "api.deepgram.com/v1/listen") != nullptr;
}

void makeDeepgramUpload(size_t pcmBytes)
{
    // Deepgram's prerecorded /listen endpoint accepts the WAV as the raw request body,
    // unlike the OpenAI-compatible multipart transcription endpoint. Transfer the WAV buffer
    // directly; tdeck_net_post_bytes_owned() takes ownership after the request is queued.
    gUpload = gAudio;
    gUploadLen = kWavHeader + pcmBytes;
    gAudio = nullptr;
    gUploadReady = true;
}

void recordTask(void *)
{
    const bool codecOk = initCodec();
    if (!codecOk) {
        heap_caps_free(gAudio);
        gAudio = nullptr;
        setStatus("ES7210 microphone setup failed");
        gState = FAILED;
        gTask = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    bool i2sOk = initI2s();
    if (!i2sOk) {
        stopCodec();
        if (gAudio) {
            heap_caps_free(gAudio);
            gAudio = nullptr;
        }
        setStatus("Microphone I2S could not start");
        gState = FAILED;
        gTask = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    size_t got = 0;
    uint8_t readErrors = 0;
    uint32_t levelLogAtMs = millis();
    int64_t slotSum[2] = {};
    uint64_t slotSumSquares[2] = {};
    uint32_t slotAbs[2] = {};
    uint32_t slotCount[2] = {};
    int16_t slotMin[2] = {INT16_MAX, INT16_MAX};
    int16_t slotMax[2] = {INT16_MIN, INT16_MIN};
    uint8_t raw[1024];
    const bool diagnostic = gDiagnostic;
    const size_t captureCapacityBytes = gCaptureCapacityBytes;
    const uint32_t captureStartedMs = millis();
    gCaptureStartedMs = captureStartedMs;
    const uint32_t allocatedSeconds = (uint32_t)(captureCapacityBytes / (kSampleRate * 2));
    const uint32_t maxCaptureSeconds = diagnostic ? kMaxDiagnosticSeconds : allocatedSeconds;
    const uint32_t captureLimitMs = (maxCaptureSeconds * 1000U) + 1500U;
    while (!gStopRequested && got < captureCapacityBytes) {
        size_t n = 0;
        const esp_err_t err = i2s_read(kI2sPort, raw, sizeof(raw), &n,
                                       pdMS_TO_TICKS(300));
        if (err != ESP_OK) {
            if (++readErrors >= 3) {
                LOG_ERROR("[VOICE] repeated I2S read error: %s", esp_err_to_name(err));
                break;
            }
        } else if (n > 0) {
            readErrors = 0;
            uint32_t outBytes = 0;
            const size_t frames = n / (2U * sizeof(int16_t));
            uint32_t blockAbs[2] = {};
            for (size_t frame = 0; frame < frames; frame++) {
                for (size_t ch = 0; ch < 2; ch++) {
                    const size_t off = (frame * 2 + ch) * sizeof(int16_t);
                    const int16_t sample = (int16_t)((uint16_t)raw[off] | ((uint16_t)raw[off + 1] << 8));
                    blockAbs[ch] += sample < 0 ? (uint32_t)(-(int32_t)sample) : (uint32_t)sample;
                }
            }
                const uint8_t inputMode = gMicInputMode;
                const size_t selected = inputMode == 1 ? 0 : inputMode == 2 ? 1 :
                                        (blockAbs[1] > blockAbs[0] ? 1 : 0);
            for (size_t frame = 0; frame < frames && got + outBytes + 2 <= captureCapacityBytes; frame++) {
                int16_t slots[2];
                for (size_t ch = 0; ch < 2; ch++) {
                    const size_t off = (frame * 2 + ch) * sizeof(int16_t);
                    slots[ch] = (int16_t)((uint16_t)raw[off] | ((uint16_t)raw[off + 1] << 8));
                    slotSum[ch] += slots[ch];
                    slotSumSquares[ch] += (uint64_t)((int64_t)slots[ch] * slots[ch]);
                    const uint32_t magnitude = slots[ch] < 0 ? (uint32_t)(-(int32_t)slots[ch]) : (uint32_t)slots[ch];
                    slotAbs[ch] += magnitude;
                    slotCount[ch]++;
                    if (slots[ch] < slotMin[ch])
                        slotMin[ch] = slots[ch];
                    if (slots[ch] > slotMax[ch])
                        slotMax[ch] = slots[ch];
                }
                // Use the louder input for this DMA block; averaging the mics could
                // cancel speech when their acoustic phase differs.
                const int16_t mono = slots[selected];
                uint8_t *dst = gAudio + kWavHeader + got + outBytes;
                dst[0] = (uint8_t)(mono & 0xff);
                dst[1] = (uint8_t)(((uint16_t)mono >> 8) & 0xff);
                outBytes += 2;
            }
            const size_t samples = outBytes / sizeof(int16_t);
            const uint8_t levelMic1 = frames ? clampLevel(blockAbs[0] / frames) : 0;
            const uint8_t levelMic2 = frames ? clampLevel(blockAbs[1] / frames) : 0;
            gInputLevelMic1 = (uint8_t)((gInputLevelMic1 * 2U + levelMic1) / 3U);
            gInputLevelMic2 = (uint8_t)((gInputLevelMic2 * 2U + levelMic2) / 3U);
            gInputLevel = max(gInputLevelMic1, gInputLevelMic2);
            got += outBytes;
            if ((uint32_t)(millis() - levelLogAtMs) >= 2000) {
                LOG_INFO("[VOICE] MIC1(left) rms=%u mean=%ld min=%d max=%d | MIC2(right) rms=%u mean=%ld min=%d max=%d | level=%u",
                         slotCount[0] ? (unsigned)sqrt((double)slotSumSquares[0] / slotCount[0]) : 0,
                         slotCount[0] ? (long)(slotSum[0] / slotCount[0]) : 0,
                         slotCount[0] ? slotMin[0] : 0, slotCount[0] ? slotMax[0] : 0,
                         slotCount[1] ? (unsigned)sqrt((double)slotSumSquares[1] / slotCount[1]) : 0,
                         slotCount[1] ? (long)(slotSum[1] / slotCount[1]) : 0,
                         slotCount[1] ? slotMin[1] : 0, slotCount[1] ? slotMax[1] : 0,
                         (unsigned)gInputLevel);
                LOG_INFO("[VOICE] channel average abs: MIC1=%u MIC2=%u",
                         slotCount[0] ? (unsigned)(slotAbs[0] / slotCount[0]) : 0,
                         slotCount[1] ? (unsigned)(slotAbs[1] / slotCount[1]) : 0);
                levelLogAtMs = millis();
            }
        }
        // A timeout with zero bytes is transient: keep waiting for the next DMA block, but
        // bound the whole capture so a dead codec cannot leave the UI stuck in "Recording".
        if ((uint32_t)(millis() - captureStartedMs) >= captureLimitMs)
            break;
    }
    stopI2s();
    // Keep the ES7210 configured between recordings. The I2S RX driver is closed above, so
    // there is no capture stream or file write while idle; the next recording reuses the codec.
    if (got < 1600) {
        heap_caps_free(gAudio);
        gAudio = nullptr;
        setStatus("No microphone audio captured");
        gState = FAILED;
        gTask = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    gCaptureStartedMs = 0;
    if (diagnostic) {
        if (gDiagnosticAudio)
            heap_caps_free(gDiagnosticAudio);
        writeWavHeader(gAudio, got);
        gDiagnosticAudio = gAudio;
        gDiagnosticAudioLen = kWavHeader + got;
        gAudio = nullptr;
        setStatus("Aufnahme fertig - Abhören möglich");
        gState = DONE;
        gTask = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    int32_t peak = 0;
    double sumSquares = 0.0;
    const size_t samples = got / 2;
    for (size_t i = 0; i < samples; i++) {
        const size_t off = kWavHeader + i * 2;
        int32_t sample = (int16_t)((uint16_t)gAudio[off] | ((uint16_t)gAudio[off + 1] << 8));
        // The T-Deck MEMS microphones are quiet. Add 6 dB of digital makeup gain after the
        // ES7210's conservative analog gain, with saturation so loud syllables cannot wrap.
        sample *= 2;
        if (sample > INT16_MAX)
            sample = INT16_MAX;
        else if (sample < INT16_MIN)
            sample = INT16_MIN;
        const int16_t gained = (int16_t)sample;
        gAudio[off] = (uint8_t)(gained & 0xff);
        gAudio[off + 1] = (uint8_t)(((uint16_t)gained >> 8) & 0xff);
        const int32_t magnitude = sample < 0 ? -(int32_t)sample : (int32_t)sample;
        if (magnitude > peak)
            peak = magnitude;
        sumSquares += (double)gained * (double)gained;
    }
    const int rms = samples ? (int)sqrt(sumSquares / (double)samples) : 0;
    LOG_INFO("[VOICE] captured=%u bytes, peak=%ld, rms=%d, codec=%s", (unsigned)got, (long)peak, rms,
             codecOk ? "ok" : "warning");
    char level[96];
    snprintf(level, sizeof(level), "Mic signal peak %ld / RMS %d", (long)peak, rms);
    setStatus(level);
    if (peak < 12) {
        heap_caps_free(gAudio);
        gAudio = nullptr;
        setStatus("Microphone is silent - no signal captured");
        gState = FAILED;
        gTask = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    writeWavHeader(gAudio, got);
    if (isDeepgramEndpoint())
        makeDeepgramUpload(got);
    else
        makeMultipart(got);
    heap_caps_free(gAudio);
    gAudio = nullptr;
    if (gState != FAILED) {
        setStatus("Sending audio for transcription...");
        LOG_INFO("[VOICE] upload ready: %u bytes", (unsigned)gUploadLen);
        gState = UPLOADING;
    }
    gTask = nullptr;
    vTaskDelete(nullptr);
}

void unescapeJsonText(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < cap; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n')
                dst[o++] = '\n';
            else if (*p == 'r')
                dst[o++] = '\r';
            else if (*p == 't')
                dst[o++] = '\t';
            else
                dst[o++] = *p;
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = 0;
}

bool extractText(const char *json)
{
    const char *field = isDeepgramEndpoint() ? "\"transcript\"" : "\"text\"";
    const char *p = strstr(json, field);
    if (!p)
        return false;
    p = strchr(p + strlen(field), ':');
    if (!p)
        return false;
    while (*++p == ' ')
        ;
    if (*p != '"')
        return false;
    p++;
    const char *end = p;
    bool escaped = false;
    while (*end) {
        if (!escaped && *end == '"')
            break;
        if (!escaped && *end == '\\')
            escaped = true;
        else
            escaped = false;
        end++;
    }
    if (*end != '"')
        return false;
    char tmp[700];
    const size_t n = (size_t)(end - p) < sizeof(tmp) - 1 ? (size_t)(end - p) : sizeof(tmp) - 1;
    memcpy(tmp, p, n);
    tmp[n] = 0;
    unescapeJsonText(tmp, gTranscript, sizeof(gTranscript));
    return gTranscript[0] != 0;
}

void applyTranscriptToFocused(void *)
{
    gApplyPending = false;
    lv_obj_t *target = gTarget;
    if (!target || !lv_obj_is_valid(target) || !lv_obj_check_type(target, &lv_textarea_class)) {
        lv_group_t *group = lv_group_get_default();
        target = group ? lv_group_get_focused(group) : nullptr;
    }
    if (target && lv_obj_check_type(target, &lv_textarea_class)) {
        lv_textarea_add_text(target, gTranscript);
        lv_display_trigger_activity(nullptr);
    }
}
} // namespace

bool startRecording()
{
    if (gState == RECORDING && gTask) {
        gStopRequested = true;
        gState = UPLOADING;
        setStatus("Recording stopped - transcribing...");
        return true;
    }
    if (!gVoiceEnabled || gTask || gState == UPLOADING || gPlaybackStarted)
        return false;
    if (!tdeckgemini::voiceConfig(gUrl, sizeof(gUrl), gKey, sizeof(gKey), gModel, sizeof(gModel), gLanguage,
                                  sizeof(gLanguage))) {
        setStatus("Deepgram key missing: add key=... to /deepgram.txt");
        gState = FAILED;
        return false;
    }
    // Preserve the editor before the physical key or Speak button changes focus.
    if (!gTarget || !lv_obj_is_valid(gTarget)) {
        lv_group_t *group = lv_group_get_default();
        lv_obj_t *focused = group ? lv_group_get_focused(group) : nullptr;
        if (focused && lv_obj_check_type(focused, &lv_textarea_class))
            gTarget = focused;
    }
    gTranscript[0] = 0;
    gDiagnostic = false;
    gStopRequested = false;
    gInputLevel = 0;
    gInputLevelMic1 = 0;
    gInputLevelMic2 = 0;
    const size_t largestPsramBlock = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t usablePsram = largestPsramBlock > kWavHeader + kRecordPsramReserveBytes
                             ? largestPsramBlock - kWavHeader - kRecordPsramReserveBytes
                             : 0;
    if (usablePsram > kRecordAudioBytes)
        usablePsram = kRecordAudioBytes;
    // Keep whole 16-bit samples in the buffer.
    usablePsram &= ~(size_t)1;
    gCaptureCapacityBytes = usablePsram;
    gCaptureStartedMs = 0;
    const unsigned captureSeconds = (unsigned)(gCaptureCapacityBytes / (kSampleRate * 2));
    LOG_INFO("[VOICE] allocating up to %u seconds (%u audio bytes), PSRAM free=%u largest=%u",
             captureSeconds, (unsigned)gCaptureCapacityBytes,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)largestPsramBlock);
    if (gCaptureCapacityBytes < kMinimumCaptureBytes) {
        gCaptureCapacityBytes = 0;
        setStatus("Not enough PSRAM - close apps and retry");
        gState = FAILED;
        return false;
    }
    gAudio = (uint8_t *)heap_caps_malloc(kWavHeader + gCaptureCapacityBytes, MALLOC_CAP_SPIRAM);
    if (!gAudio) {
        // Heap state can change between the size query and allocation. Retry with
        // progressively smaller recordings instead of rejecting voice input outright.
        while (gCaptureCapacityBytes > kMinimumCaptureBytes) {
            gCaptureCapacityBytes -= 256 * 1024;
            gCaptureCapacityBytes &= ~(size_t)1;
            if (gCaptureCapacityBytes < kMinimumCaptureBytes)
                gCaptureCapacityBytes = kMinimumCaptureBytes;
            gAudio = (uint8_t *)heap_caps_malloc(kWavHeader + gCaptureCapacityBytes, MALLOC_CAP_SPIRAM);
            if (gAudio)
                break;
        }
    }
    if (!gAudio) {
        gCaptureCapacityBytes = 0;
        setStatus("Not enough PSRAM - close apps and retry");
        gState = FAILED;
        return false;
    }
    setStatus("Recording - press mic again to stop");
    gState = RECORDING;
    if (xTaskCreatePinnedToCore(recordTask, "tdeck_voice", 8192, nullptr, 2, &gTask, 1) != pdPASS) {
        heap_caps_free(gAudio);
        gAudio = nullptr;
        gTask = nullptr;
        setStatus("Could not start microphone task");
        gState = FAILED;
        return false;
    }
    return true;
}

bool startMonitor()
{
    if (gState == RECORDING && gTask) {
        gStopRequested = true;
        setStatus("Mic-Test wird beendet...");
        return true;
    }
    if (gTask || gState == UPLOADING)
        return false;
    gStopRequested = false;
    gInputLevel = 0;
    gInputLevelMic1 = 0;
    gInputLevelMic2 = 0;
    gCaptureCapacityBytes = kDiagnosticAudioBytes;
    gCaptureStartedMs = 0;
    gDiagnostic = true;
    gAudio = (uint8_t *)heap_caps_malloc(kWavHeader + gCaptureCapacityBytes, MALLOC_CAP_SPIRAM);
    if (!gAudio) {
        gDiagnostic = false;
        setStatus("Nicht genug Speicher für Mic-Test");
        gState = FAILED;
        return false;
    }
    setStatus("Mic-Test läuft - sprechen");
    gState = RECORDING;
    if (xTaskCreatePinnedToCore(recordTask, "tdeck_mic_test", 8192, nullptr, 2, &gTask, 1) != pdPASS) {
        heap_caps_free(gAudio);
        gAudio = nullptr;
        gTask = nullptr;
        gDiagnostic = false;
        setStatus("Mic-Test konnte nicht starten");
        gState = FAILED;
        return false;
    }
    return true;
}

bool diagnosticPlaybackAvailable()
{
    return gDiagnosticAudio != nullptr && gDiagnosticAudioLen > kWavHeader;
}

bool playDiagnosticRecording()
{
    if (!diagnosticPlaybackAvailable() || !audioThread || gPlaybackPending || gPlaybackStarted)
        return false;
    gPlaybackPending = true;
    setStatus("Wiedergabe wird gestartet...");
    return true;
}

void setTarget(lv_obj_t *target)
{
    if (target && lv_obj_is_valid(target) && lv_obj_check_type(target, &lv_textarea_class))
        gTarget = target;
}

void service()
{
    loadAudioSettings();
    if (gMicGainSavePending || gMicInputModeSavePending || gSpeakerVolumeSavePending) {
        Preferences prefs;
        if (prefs.begin("tdeckaudio", false)) {
            if (gMicGainSavePending) {
                gMicGainSavePending = false;
                prefs.putUChar("mic1_gain", gMicGainIndex[0]);
                prefs.putUChar("mic2_gain", gMicGainIndex[1]);
            }
            if (gMicInputModeSavePending) {
                gMicInputModeSavePending = false;
                prefs.putUChar("mic_input", gMicInputMode);
            }
            if (gSpeakerVolumeSavePending) {
                gSpeakerVolumeSavePending = false;
                prefs.putUChar("speaker", gSpeakerVolume);
            }
            prefs.end();
        }
    }
    if (gSpeakerGainApplyPending && audioThread) {
        gSpeakerGainApplyPending = false;
        audioThread->setGain((float)gSpeakerVolume / 100.0f);
    }
    if (gPlaybackPending) {
        gPlaybackPending = false;
        if (audioThread && !audioThread->wavPlaying()) {
            audioThread->beginWav(gDiagnosticAudio, (uint32_t)gDiagnosticAudioLen);
            gPlaybackStarted = audioThread->wavPlaying();
            setStatus(gPlaybackStarted ? "Aufnahme wird über Lautsprecher abgespielt" : "Lautsprecher ist belegt");
        }
    }
    if (gPlaybackStarted && audioThread && !audioThread->wavPlaying()) {
        gPlaybackStarted = false;
        if (gDiagnosticAudio) {
            heap_caps_free(gDiagnosticAudio);
            gDiagnosticAudio = nullptr;
            gDiagnosticAudioLen = 0;
        }
        setStatus("Wiedergabe beendet - neuer Pegeltest möglich");
    }
    if (tdeck_voice_request) {
        tdeck_voice_request = false;
        if (!startRecording())
            ; // statusText() contains the actionable reason for the UI
    }
    if (gUploadReady && gState == UPLOADING) {
        gUploadReady = false;
        char auth[128];
        snprintf(auth, sizeof(auth), "%s %s", isDeepgramEndpoint() ? "Token" : "Bearer", gKey);
        tdeck_net_set_auth(auth);
        char contentType[96];
        char requestUrl[384];
        if (isDeepgramEndpoint())
            snprintf(contentType, sizeof(contentType), "audio/wav");
        else
            snprintf(contentType, sizeof(contentType), "multipart/form-data; boundary=----TDeckVoiceBoundary7MA4YWxkTrZu0gW");
        if (isDeepgramEndpoint()) {
            const char separator = strchr(gUrl, '?') ? '&' : '?';
            snprintf(requestUrl, sizeof(requestUrl), "%s%cmodel=%s&language=%s&smart_format=true&punctuate=true",
                     gUrl, separator, gModel[0] ? gModel : "nova-3", gLanguage[0] ? gLanguage : "de");
        } else {
            snprintf(requestUrl, sizeof(requestUrl), "%s", gUrl);
        }
        const bool deepgram = isDeepgramEndpoint();
        const bool queued = deepgram
                                ? tdeck_net_post_bytes_owned(requestUrl, gUpload, gUploadLen, contentType)
                                : tdeck_net_post_bytes(requestUrl, gUpload, gUploadLen, contentType);
        if (!queued) {
            heap_caps_free(gUpload);
            gUpload = nullptr;
            gUploadLen = 0;
            setStatus("Network is busy - try voice again");
            gState = FAILED;
        } else {
            gUploadStartedMs = millis();
            LOG_INFO("[VOICE] POST queued to transcription service");
            if (!deepgram)
                heap_caps_free(gUpload);
            gUpload = nullptr;
            gUploadLen = 0;
        }
    }
    if (gState != UPLOADING)
        return;
    if (gUploadStartedMs && (uint32_t)(millis() - gUploadStartedMs) > 180000) {
        LOG_INFO("[VOICE] upload timeout after 180 seconds");
        tdeck_net_reset();
        setStatus("Speech upload timed out");
        gState = FAILED;
        gUploadStartedMs = 0;
        return;
    }
    const int net = tdeck_net_poll();
    if (net == 2) {
        char result[kResultCap];
        const int n = tdeck_net_result(result, sizeof(result));
        tdeck_net_reset();
        if (n > 0 && extractText(result)) {
            setStatus("Voice text ready - press Ask");
            if (!gApplyPending) {
                gApplyPending = true;
                lv_async_call(applyTranscriptToFocused, nullptr);
            }
            gState = DONE;
        } else {
            setStatus("Transcription returned no text");
            gState = FAILED;
        }
    } else if (net == 3) {
        const int code = tdeck_net_http_code();
        tdeck_net_reset();
        char msg[96];
        snprintf(msg, sizeof(msg), "Speech service error %d", code);
        setStatus(msg);
        gState = FAILED;
    }
}

int state() { return (int)gState; }
bool voiceEnabled() { loadAudioSettings(); return gVoiceEnabled; }
void setVoiceEnabled(bool enabled)
{
    loadAudioSettings();
    gVoiceEnabled = enabled;
    Preferences prefs;
    if (prefs.begin("tdeckaudio", false)) {
        prefs.putBool("voice_enabled", enabled);
        prefs.end();
    }
}
uint32_t recordingElapsedSeconds()
{
    if (gState != RECORDING || !gCaptureStartedMs)
        return 0;
    return (uint32_t)(millis() - gCaptureStartedMs) / 1000U;
}
uint32_t recordingRemainingSeconds()
{
    if (gState != RECORDING)
        return 0;
    const uint32_t available = (uint32_t)(gCaptureCapacityBytes / (kSampleRate * 2));
    if (!gCaptureStartedMs)
        return available;
    const uint32_t elapsed = recordingElapsedSeconds();
    return elapsed < available ? available - elapsed : 0;
}
uint8_t inputLevel() { return gInputLevel; }
uint8_t inputLevelMic1() { return gInputLevelMic1; }
uint8_t inputLevelMic2() { return gInputLevelMic2; }
uint8_t micGainIndex(uint8_t channel) { return gMicGainIndex[channel == 2 ? 1 : 0]; }
float micGainDb(uint8_t channel)
{
    static const float db[kMicGainCount] = {0.0f, 3.0f, 6.0f, 9.0f, 12.0f, 15.0f, 18.0f, 21.0f,
                                            24.0f, 27.0f, 30.0f, 33.0f, 34.5f, 36.0f, 37.5f};
    return db[gMicGainIndex[channel == 2 ? 1 : 0]];
}
void setMicGainIndex(uint8_t channel, uint8_t index, bool save)
{
    const uint8_t selected = channel == 2 ? 1 : 0;
    if (index >= kMicGainCount)
        index = kMicGainCount - 1;
    gMicGainIndex[selected] = index;
    if (save)
        gMicGainSavePending = true;
}
uint8_t inputMode() { return gMicInputMode; }
void setInputMode(uint8_t mode, bool save)
{
    gMicInputMode = mode > 2 ? 0 : mode;
    if (save)
        gMicInputModeSavePending = true;
}
uint8_t speakerVolume() { return gSpeakerVolume; }
void setSpeakerVolume(uint8_t percent, bool save)
{
    gSpeakerVolume = percent > 100 ? 100 : percent;
    gSpeakerGainApplyPending = true;
    if (save)
        gSpeakerVolumeSavePending = true;
}
const char *statusText() { return gStatus; }
const char *transcript() { return gTranscript; }
} // namespace tdeckvoice
