// -----------------------------------------------------------------------------
// T-Deck launcher: Fahrenheit or Celsius (firmware side).
//
// Jake: "can we have an option of F or C on the weather widget."
//
// ⭐ THE SETTING ALREADY EXISTED - it just had no switch. The lock-screen weather widget has
// always honoured Meshtastic's own config.display.units (see lockwidget_fill's `celsius`
// argument), and so do the node metrics and the environment readings. What was missing was any
// way to change it from the device: it could only be set from the phone app.
//
// So this is a BRIDGE, not a new preference, for the same reason TDeckClockFormat.cpp is one.
// Adding a second, widget-local unit setting would have been less work and worse: the widget
// would say 18C while the node list two taps away said 64F, and there would be no answer to
// "which units is this device on". Driving Meshtastic's own field keeps the phone app, the
// widget and every other screen agreeing.
//
// THREADING: the same deferred pattern as the clock format and the sound toggle. The UI runs on
// its own FreeRTOS task and writing settings to flash from there is what froze the device when
// the time-zone dropdown first tried it (see TDeckTimeZone.cpp). The UI-facing setter records
// intent only; tdeck_units_service(), called from loop(), does the write on the safe thread.
// -----------------------------------------------------------------------------
#include "main.h"
#include "mesh/NodeDB.h" // the global `config` + nodeDB->saveToDisk

static volatile bool s_unitsPending = false;
static volatile bool s_unitsWantMetric = false;

// Live truth for the Settings row.
extern "C" bool tdeck_units_metric(void)
{
    return config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_METRIC;
}

// Called from the UI (LVGL) task - records intent only.
extern "C" void tdeck_units_set_metric(bool metric)
{
    s_unitsWantMetric = metric;
    s_unitsPending = true;
}

// Called from the main Meshtastic loop(). Cheap when idle.
extern "C" void tdeck_units_service(void)
{
    if (!s_unitsPending)
        return;
    s_unitsPending = false;
    config.display.units = s_unitsWantMetric ? meshtastic_Config_DisplayConfig_DisplayUnits_METRIC
                                             : meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL;
    nodeDB->saveToDisk(SEGMENT_CONFIG); // persists without a reboot
}
