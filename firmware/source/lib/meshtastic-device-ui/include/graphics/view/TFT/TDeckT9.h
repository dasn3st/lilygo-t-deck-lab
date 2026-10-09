#pragma once

// Lightweight offline word suggestions for every LVGL text field on the T-Deck.
// The helper deliberately lives at the UI boundary so Notes, Ollama, Terminal,
// Calendar and the other apps all get the same behaviour.

bool tdeck_t9_enabled(void);
void tdeck_t9_set_enabled(bool enabled);
void tdeck_t9_tick(void);
