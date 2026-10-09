// -----------------------------------------------------------------------------
// TDeckBeep — firmware-side bridge letting the device-ui set the audio output
// gain (loudness) across the UI/firmware boundary, so the timer alarm can play
// much louder than the gentle default. Same linker-resolved pattern as
// TDeckMeshSwitch / TDeckGpsBridge / TDeckMemInfo.
// -----------------------------------------------------------------------------
#include "main.h"
#include "TDeckVoice.h"
#include "mesh/NodeDB.h" // config.device.buzzer_mode + nodeDB->saveToDisk

#ifdef HAS_I2S
#include "AudioThread.h"
#endif

// g = 0.0..1.0 (default output gain is 0.2). The timer sets ~1.0 for a loud
// alarm, then restores 0.2 afterwards. No-op if there's no audio thread.
extern "C" void tdeck_beep_gain(float g)
{
#ifdef HAS_I2S
    if (audioThread)
        audioThread->setGain(g);
#else
    (void)g;
#endif
}

extern "C" void tdeck_beep_restore_gain(void)
{
#ifdef HAS_I2S
    if (audioThread)
        audioThread->setGain((float)tdeckvoice::speakerVolume() / 100.0f);
#endif
}

// --- Sound on/off (Settings toggle): drives Meshtastic's OWN buzzer_mode, so ONE switch
// governs everything — the system beeps (games/metronome/timer/Lua, gated inside buzz.cpp's
// playTones) AND the message-notification ringtone (gated in ExternalNotificationModule).
// Persists in the device config, so phone apps see and can change the same setting.
// (Replaces the old separate NVS mute, which silenced beeps but not message alerts.)
// Applied via a deferred service on the main loop (same pattern as the GPS/mesh bridges)
// so the UI task never writes the filesystem itself.
static volatile bool s_soundPending = false;
static volatile bool s_soundWantOn = true;

// Live truth for the Settings switch: anything but DISABLED counts as "sound on".
extern "C" bool tdeck_sound_get_enabled(void)
{
    return config.device.buzzer_mode != meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED;
}

// Called from the UI (LVGL) task — records intent only.
extern "C" void tdeck_sound_set_enabled(bool on)
{
    s_soundWantOn = on;
    s_soundPending = true;
}

// Called from the main Meshtastic loop(). Cheap when idle.
extern "C" void tdeck_sound_service(void)
{
    if (!s_soundPending)
        return;
    s_soundPending = false;
    config.device.buzzer_mode = s_soundWantOn ? meshtastic_Config_DeviceConfig_BuzzerMode_ALL_ENABLED
                                              : meshtastic_Config_DeviceConfig_BuzzerMode_DISABLED;
    nodeDB->saveToDisk(SEGMENT_CONFIG); // persists without a reboot
}
