#pragma once
// One shape for all three lock-screen widgets: a headline, a word beside it, and two quiet
// lines underneath. See source/graphics/TFT/LockWidgets.cpp for the why.
#include <cstddef>
#include <cstdint>

struct LockWidgetText {
    char big[24];   // headline, drawn in montserrat_20
    char tag[24];   // the word beside the headline ("sunset", "locked", "Rain showers")
    uint32_t tagColor;
    char line1[48]; // the useful line
    char line2[64]; // the quiet line
    char right[24]; // right-aligned beside line1, or empty
};

// which: 1 = weather, 2 = satellites, 3 = sun. Returns false for anything else (including
// "None"), so the caller can simply not draw a card.
// WARNING: tft task only - it reads the SD card and relies on task_handler()'s spiLock.
bool lockwidget_fill(int which, bool celsius, bool ampm, LockWidgetText *out);
