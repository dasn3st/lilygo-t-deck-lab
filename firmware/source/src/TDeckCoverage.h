#pragma once

#include <stdbool.h>
#include <stdint.h>

// Mesh coverage mapper. Jake, 2026-09-22: picked from a shortlist, and the reason it is worth
// building is that he runs two solar relay nodes and has no way at all to know whether they are
// doing anything, or where a third should go.
//
// Every packet the radio receives already carries RSSI and SNR. Pair that with the GPS fix and
// you get "the mesh reaches HERE, this well" - walk or drive around and the map fills in.
//
// See TDeckCoverage.cpp for why this bins into a grid instead of logging every packet.

#ifdef __cplusplus
extern "C" {
#endif

// Is recording on? Off by default - it costs GPS power and SD writes, so it is a deliberate act.
bool tdeck_coverage_enabled(void);
void tdeck_coverage_set_enabled(bool on);

// ⚠️ CALLED FROM THE PACKET PATH, WHICH IS THE "tft" TASK HOLDING spiLock. It must stay a
// handful of microseconds: a hash insert and nothing else. No SD, no logging, no allocation.
void tdeck_coverage_sample(uint32_t fromNode, int32_t rssi, float snr);

// Called from loop() in main.cpp. Does the SD writing, well away from the UI task.
void tdeck_coverage_service(void);

// How many grid cells have been recorded.
int tdeck_coverage_count(void);

// Read cell i back for drawing. lat/lon are the cell CENTRE in 1e-7 degrees, snrQ is the best
// SNR seen there in quarter-dB (so -40 means -10.0 dB), n is how many packets landed in it.
bool tdeck_coverage_cell(int i, int32_t *lat, int32_t *lon, int *snrQ, int *rssi, int *n);

// Wipe the grid and the file. Used by the "clear" button, and after a move to a new area.
void tdeck_coverage_clear(void);

// ⛔ TEST DATA ONLY, and it POISONS the grid on purpose: once called, nothing is saved to the
// card until tdeck_coverage_clear(). Exists so the heatmap drawing can be proven at a desk
// instead of by driving around and hoping. Fake coverage written to /coverage.csv would be the
// worst bug this feature could have, so the block is a flag, not a convention.
void tdeck_coverage_inject(int32_t lat, int32_t lon, int snrQ, int rssi);
bool tdeck_coverage_is_synthetic(void);

// Load a previously saved grid from the SD card. Safe to call when there is no file.
void tdeck_coverage_load(void);

#ifdef __cplusplus
}
#endif
