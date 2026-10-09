// -----------------------------------------------------------------------------
// T-Deck launcher: keyboard backlight follows the screen (firmware side).
//
// Requested by a web-installer user: "an option to turn on the keyboard backlight
// when the screen is on". The keys are unlit by default, so the one time you most
// want them — in the dark — you can't see them without remembering Alt+B.
//
// How the hardware works: the keys are not wired to the main ESP32-S3 at all. They
// hang off a second little chip (an ESP32-C3) that acts as an I2C slave at
// TDECK_KB_ADDR (0x55) and reports the ASCII of the last key pressed. That chip owns
// the backlight and handles the Alt+B toggle entirely by itself — which is why the
// main firmware never saw the backlight state and couldn't tie it to the screen.
// It does accept two commands, though:
//     0x01 <0-255>   set backlight brightness now (0 = off)
//     0x02 <30-255>  set the brightness Alt+B restores
// We only use 0x01, so the user's own Alt+B default is left exactly as they set it.
//
// DEFAULT OFF. This writes to a chip whose firmware version we can't see (people
// reflash these), and an unlit keyboard is what every existing device already does,
// so it's opt-in from Settings rather than a surprise change. Flag lives in NVS for
// the same reason as the lock switch — see TDeckLockControl.cpp.
// -----------------------------------------------------------------------------
#include "configuration.h"
#include <Preferences.h>
#include <Wire.h>

// Mid-scale: bright enough to read the keys in the dark without lighting up a room
// or noticeably adding to the battery draw. The keyboard clamps its own range.
static const uint8_t kKbdLightOn = 140;

static int s_kbdLightEnabled = -1; // -1 = not read from NVS yet; 0 = off; 1 = on
static int s_kbdLightState = -1;   // last brightness we actually sent; -1 = unknown

extern "C" bool tdeck_kbdlight_enabled(void);
extern "C" void tdeck_kbdlight_screen(bool screenOn);

// Talk to the keyboard controller. Cheap, but never called on a hot path: every
// caller below is edge-triggered, so this only runs when the state actually changes.
static void kbdLightWrite(uint8_t brightness)
{
    Wire.beginTransmission(TDECK_KB_ADDR);
    Wire.write(0x01); // LILYGO_KB_BRIGHTNESS_CMD (confirmed against LilyGO's own keyboard firmware)
    Wire.write(brightness);
    uint8_t rc = Wire.endTransmission();
    // rc == 0 means the keyboard acknowledged the command; 2 or 3 mean it did NOT — which on a
    // keyboard running older firmware (only Alt+B, no I2C brightness command) is expected. Logging
    // it lets a serial capture tell "the command is wrong" apart from "this keyboard can't do it".
    LOG_INFO("kbdlight: brightness=%u i2c_rc=%u", (unsigned)brightness, (unsigned)rc);
}

extern "C" bool tdeck_kbdlight_enabled(void)
{
    if (s_kbdLightEnabled < 0) {
        Preferences p;
        if (p.begin("tdeckkbl", true)) { // read-only
            s_kbdLightEnabled = p.getBool("en", false) ? 1 : 0;
            p.end();
        } else {
            s_kbdLightEnabled = 0; // namespace not created yet -> stay off, as before
        }
    }
    return s_kbdLightEnabled != 0;
}

extern "C" void tdeck_kbdlight_set_enabled(bool en)
{
    s_kbdLightEnabled = en ? 1 : 0;
    Preferences p;
    if (p.begin("tdeckkbl", false)) { // read-write
        p.putBool("en", en);
        p.end();
    }
    // Act on it immediately: the user is looking at a lit screen in Settings, so
    // switching on means "light up now", and switching off must not leave the keys
    // lit with nothing left to turn them off but Alt+B.
    s_kbdLightState = -1; // force the write through even if the cached state matches
    tdeck_kbdlight_screen(en);
}

// Called when the screen lights up or goes dark. Does nothing unless the option is on,
// and nothing at all if the keyboard is already in the requested state.
extern "C" void tdeck_kbdlight_screen(bool screenOn)
{
    if (!tdeck_kbdlight_enabled()) {
        // Turn the light off once on the way out, then stay quiet. Without this, a
        // device that had the option on and the keys lit would keep them lit forever
        // after the option was switched off.
        if (s_kbdLightState > 0) {
            kbdLightWrite(0);
            s_kbdLightState = 0;
        }
        return;
    }
    int want = screenOn ? kKbdLightOn : 0;
    if (s_kbdLightState == want)
        return;
    s_kbdLightState = want;
    kbdLightWrite((uint8_t)want);
}
