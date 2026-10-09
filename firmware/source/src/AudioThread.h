#pragma once
#include "PowerFSM.h"
#include "concurrency/OSThread.h"
#include "configuration.h"
#include "main.h"
#include "sleep.h"
#include <memory>

#ifdef HAS_I2S
#include <AudioFileSourcePROGMEM.h>
#include <AudioGeneratorRTTTL.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>
#include <ESP8266SAM.h>

#ifdef USE_XL9555
#include "ExtensionIOXL9555.hpp"
extern ExtensionIOXL9555 io;
#endif

#define AUDIO_THREAD_INTERVAL_MS 100

class AudioThread : public concurrency::OSThread
{
  public:
    AudioThread() : OSThread("Audio") { initOutput(); }

    void beginRttl(const void *data, uint32_t len)
    {
#ifdef T_LORA_PAGER
        io.digitalWrite(EXPANDS_AMP_EN, HIGH);
#endif
        setCPUFast(true);
        rtttlFile = std::unique_ptr<AudioFileSourcePROGMEM>(new AudioFileSourcePROGMEM(data, len));
        i2sRtttl = std::unique_ptr<AudioGeneratorRTTTL>(new AudioGeneratorRTTTL());
        i2sRtttl->begin(rtttlFile.get(), audioOut.get());
    }

    // Also handles actually playing the RTTTL, needs to be called in loop
    bool isPlaying()
    {
        if (i2sRtttl != nullptr) {
            return i2sRtttl->isRunning() && i2sRtttl->loop();
        }
        return false;
    }

    void stop()
    {
        if (i2sRtttl != nullptr) {
            i2sRtttl->stop();
            i2sRtttl = nullptr;
        }

        rtttlFile = nullptr;

        setCPUFast(false);
#ifdef T_LORA_PAGER
        io.digitalWrite(EXPANDS_AMP_EN, LOW);
#endif
    }

    // Set output gain (0.0–1.0+). Default is 0.2; the timer alarm cranks this up
    // for a much louder beep, then restores it. >1.0 clips (harsher, even louder).
    void setGain(float g)
    {
        if (audioOut)
            audioOut->SetGain(g);
    }

    // Fire-and-forget WAV from flash, for the new-message pop (src/pop_wav.h).
    //
    // Deliberately NOT like beginRttl + the caller spinning on isPlaying(): that blocks
    // whoever called it, and the one thread that must never block here is the radio.
    // Stalls of RadioIf waiting on a lock are exactly what has been rebooting this
    // device. So this only STARTS playback; runOnce() below pumps it to the end and
    // tidies up, and nobody waits.
    void beginWav(const void *data, uint32_t len)
    {
        if (i2sWav != nullptr) // a pop already sounding: let it finish rather than stutter
            return;
#ifdef T_LORA_PAGER
        io.digitalWrite(EXPANDS_AMP_EN, HIGH);
#endif
        setCPUFast(true);
        wavFile = std::unique_ptr<AudioFileSourcePROGMEM>(new AudioFileSourcePROGMEM(data, len));
        i2sWav = std::unique_ptr<AudioGeneratorWAV>(new AudioGeneratorWAV());
        if (!i2sWav->begin(wavFile.get(), audioOut.get()))
            endWav();
    }

    bool wavPlaying() const { return i2sWav != nullptr; }

    void readAloud(const char *text)
    {
        if (i2sRtttl != nullptr) {
            i2sRtttl->stop();
            i2sRtttl = nullptr;
        }

#ifdef T_LORA_PAGER
        io.digitalWrite(EXPANDS_AMP_EN, HIGH);
#endif
        auto sam = std::unique_ptr<ESP8266SAM>(new ESP8266SAM);
        sam->Say(audioOut.get(), text);
        setCPUFast(false);
#ifdef T_LORA_PAGER
        io.digitalWrite(EXPANDS_AMP_EN, LOW);
#endif
    }

  protected:
    int32_t runOnce() override
    {
        canSleep = true; // Assume we should not keep the board awake

        // if (i2sRtttl != nullptr && i2sRtttl->isRunning()) {
        //     i2sRtttl->loop();
        // }

        // Pump a pop that beginWav() started. The I2S buffer needs feeding far more
        // often than every 100 ms or the sound breaks up, so while one is playing this
        // thread asks to be run again almost immediately, and only then goes back to
        // idling. canSleep stays false meanwhile so the board does not doze mid-pop.
        if (i2sWav != nullptr) {
            if (i2sWav->isRunning() && i2sWav->loop()) {
                canSleep = false;
                return 2;
            }
            endWav();
        }
        return AUDIO_THREAD_INTERVAL_MS;
    }

  private:
    void initOutput()
    {
        // LilyGO T-Deck UnitTest assigns I2S1 to the ES7210 microphone and I2S0 to the speaker.
        audioOut = std::unique_ptr<AudioOutputI2S>(new AudioOutputI2S(0, AudioOutputI2S::EXTERNAL_I2S));
        // GPIO21 is the ES7210 microphone's LRCK pin on the T-Deck. The speaker
        // only needs BCK/WS/DOUT; routing its MCLK to GPIO21 fights the mic clock.
        audioOut->SetPinout(DAC_I2S_BCK, DAC_I2S_WS, DAC_I2S_DOUT, -1);
        audioOut->SetGain(0.2);
    };

    void endWav()
    {
        if (i2sWav != nullptr) {
            i2sWav->stop();
            i2sWav = nullptr;
        }
        wavFile = nullptr;
        setCPUFast(false);
#ifdef T_LORA_PAGER
        io.digitalWrite(EXPANDS_AMP_EN, LOW);
#endif
    }

    std::unique_ptr<AudioGeneratorRTTTL> i2sRtttl = nullptr;
    std::unique_ptr<AudioGeneratorWAV> i2sWav = nullptr;
    std::unique_ptr<AudioFileSourcePROGMEM> wavFile = nullptr;
    std::unique_ptr<AudioOutputI2S> audioOut = nullptr;

    std::unique_ptr<AudioFileSourcePROGMEM> rtttlFile = nullptr;
};

#endif
