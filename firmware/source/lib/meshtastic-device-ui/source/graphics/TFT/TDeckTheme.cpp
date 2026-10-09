#include "graphics/view/TFT/TDeckTheme.h"

#include <Preferences.h>

namespace {
bool loaded = false;
bool enabled = false;

void load(void)
{
    if (loaded)
        return;
    loaded = true;
    Preferences prefs;
    if (prefs.begin("tdeckui", true)) {
        enabled = prefs.getBool("glass_icons", false);
        prefs.end();
    }
}
} // namespace

bool tdeck_glass_icons_enabled(void)
{
    load();
    return enabled;
}

void tdeck_glass_icons_set_enabled(bool value)
{
    load();
    enabled = value;
    Preferences prefs;
    if (prefs.begin("tdeckui", false)) {
        prefs.putBool("glass_icons", enabled);
        prefs.end();
    }
}
