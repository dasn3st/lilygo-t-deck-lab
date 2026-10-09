// -----------------------------------------------------------------------------
// T-Deck launcher: 12- or 24-hour clock (firmware side).
//
// Meshtastic already carries this preference (config.display.use_12h_clock) and its
// own screens honour it — but it could only be changed from the phone app, and the
// launcher's home-screen clock ignored it entirely and always drew 12-hour. A user
// in Europe reported the obvious consequence: one device showing both formats.
//
// So this is a bridge, not a new setting: the switch in Settings drives Meshtastic's
// OWN field, which means the phone app and the device agree and there's only ever one
// answer to "which format is this device on".
//
// THREADING: same deferred pattern as TDeckBeep.cpp's sound toggle. The UI runs on its
// own FreeRTOS task, and writing settings to flash from there is what froze the device
// when the time-zone dropdown first tried it (see TDeckTimeZone.cpp). The UI-facing
// setter therefore records intent only; tdeck_clock_service(), called from loop(),
// does the write on the safe thread.
// -----------------------------------------------------------------------------
#include "main.h"
#include "mesh/NodeDB.h" // the global `config` + nodeDB->saveToDisk

static volatile bool s_clockPending = false;
static volatile bool s_clockWant12h = true;

// Live truth for the Settings switch.
extern "C" bool tdeck_clock_get_12h(void)
{
    return config.display.use_12h_clock;
}

// Called from the UI (LVGL) task — records intent only.
extern "C" void tdeck_clock_set_12h(bool on)
{
    s_clockWant12h = on;
    s_clockPending = true;
}

// Called from the main Meshtastic loop(). Cheap when idle.
extern "C" void tdeck_clock_service(void)
{
    if (!s_clockPending)
        return;
    s_clockPending = false;
    config.display.use_12h_clock = s_clockWant12h;
    nodeDB->saveToDisk(SEGMENT_CONFIG); // persists without a reboot
}
