// -----------------------------------------------------------------------------
// TDeckBattLog - measure what the screen actually costs, instead of guessing.
//
// Jake asked how much battery an always-on lock screen would use. The honest answer was "I
// can't tell you": this board has no PMU, no fuel gauge and no current sensor (no HAS_PMU, no
// INA219/226/3221 in the t-deck variant), so there is no way to read milliamps. All the
// device can report is a battery voltage from an ADC.
//
// Voltage over TIME, though, is a perfectly good comparison - and it is the comparison that
// matters. Leave the device unplugged for a few hours with the screen dark, then a few hours
// with the lock screen lit at a given dim level, and the difference in millivolts-per-hour is
// the cost of that setting. That is a real number rather than my arithmetic about backlights.
//
// This writes one line a minute to /battlog.csv on the card. Read it back over the cable with
// @@batt, or just pull the card and open it in a spreadsheet.
//
// ⚠️ EXCLUDE THE CHARGING ROWS. The `usb` and `chg` columns exist so that the hours where the
// device was plugged in - which is most of the time it is on a desk - can be thrown away.
// Only unplugged rows say anything about drain.
// -----------------------------------------------------------------------------
#include "configuration.h"

#include "PowerStatus.h"
#include <Arduino.h>

// Everything the log wants from the firmware side, in one call. Returns false before the
// power subsystem has produced a reading.
extern "C" bool tdeck_battlog_sample(int *mv, int *pct, int *usb, int *charging)
{
    if (!powerStatus)
        return false;
    const int v = powerStatus->getBatteryVoltageMv();
    if (v <= 0)
        return false; // no reading yet; a zero row would be worse than no row
    if (mv)
        *mv = v;
    if (pct)
        *pct = powerStatus->getBatteryChargePercent();
    if (usb)
        *usb = powerStatus->getHasUSB() ? 1 : 0;
    if (charging)
        *charging = powerStatus->getIsCharging() ? 1 : 0;
    return true;
}
