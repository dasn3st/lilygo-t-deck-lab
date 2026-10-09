#pragma once

#include <lvgl.h>

namespace tdeckvoice
{
enum State {
    IDLE = 0,
    RECORDING,
    UPLOADING,
    DONE,
    FAILED,
};

void service();
bool startRecording();
bool voiceEnabled();
void setVoiceEnabled(bool enabled);
uint32_t recordingElapsedSeconds();
uint32_t recordingRemainingSeconds();
bool startMonitor(); // local level test; never reads Deepgram config or uploads audio
bool diagnosticPlaybackAvailable();
bool playDiagnosticRecording();
void setTarget(lv_obj_t *target);
int state();
uint8_t inputLevel();
uint8_t inputLevelMic1();
uint8_t inputLevelMic2();
uint8_t micGainIndex(uint8_t channel);
float micGainDb(uint8_t channel);
void setMicGainIndex(uint8_t channel, uint8_t index, bool save = false);
uint8_t inputMode();
void setInputMode(uint8_t mode, bool save = false);
uint8_t speakerVolume();
void setSpeakerVolume(uint8_t percent, bool save = false);
const char *statusText();
const char *transcript();
} // namespace tdeckvoice
