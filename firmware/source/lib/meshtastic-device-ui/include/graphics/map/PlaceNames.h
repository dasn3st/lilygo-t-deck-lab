#pragma once

#include <stddef.h>
#include <stdint.h>

// Offline place-name search: towns, lakes, peaks, rivers... read straight off the SD card.
//
// The data is USGS GNIS (USA, public domain) and GeoNames (Europe + world cities, CC BY 4.0 - it
// must be credited wherever results are shown). It is built on the PC by
// tdeck project/placenames/build_names.py and published at jeeab.github.io/t-ui-names, one file per
// 1x1 degree square:
//
//     /names/<floor lat>/<floor lon>.tnm      e.g. /names/47/-122.tnm
//     /names/cities.tnm                       world cities of 15,000+, searched from anywhere
//
// The on-device map downloader fetches the squares for the area it downloads; the whole of the USA
// or Europe comes as one zip from the website. search_names.py next to the builder is the reference
// implementation - this file follows it step for step, so its results are how these are checked.
//
// ⛔ SD CARD: call only from the UI task. The card shares its SPI bus with the screen and the radio,
// and the UI task is the one that holds that bus.

struct PlaceHit {
    float lat, lon;
    uint8_t kind;   // index into placenames_kind_name()
    uint8_t rank;   // cities: population on a log scale (25 per tenfold, 150 = 1 million); else 0
    uint8_t group;  // 0 = Nearby (the squares around the map), 1 = Cities (suggestions, world-wide)
    bool exact;     // one of its names is exactly what was typed
    char name[64];  // UTF-8, as the map would print it
    float score;    // lower is better, within its group
    float km;       // straight-line distance from the search centre
};

struct PlaceSearchInfo {
    uint16_t files;     // .tnm files that were there and got searched
    uint16_t reads;     // index + record reads, for tuning
    uint32_t ms;        // wall time
    bool truncated;     // stopped early on the time budget
    bool haveHere;      // the square under the search centre is on the card
    bool haveCities;    // /names/cities.tnm is on the card
    uint8_t nNear;      // how many of the hits are Nearby ...
    uint8_t nCities;    // ... and how many are Cities
    bool citiesFirst;   // the Cities come first in the list (a big city, and nothing nearby exact)
};

// Search around (lat, lon). `query` is what was typed; case and punctuation do not matter, and each
// word matches the START of a word in the name, in any order ("washington lake" finds Lake
// Washington). Two groups come back, one after the other, and info says which is first:
//   Nearby - from 3 letters: the squares around (lat, lon), nearest first
//   Cities - from 2 letters: suggestions world-wide, bigger and closer first ("san" -> San Jose,
//            San Francisco, San Diego...)
// Returns the number of hits written to `out`.
int placenames_search(const char *query, float lat, float lon, PlaceHit *out, int maxOut, uint32_t budgetMs,
                      PlaceSearchInfo *info);

// "town", "lake", "peak"... for the grey line under a result.
const char *placenames_kind_name(uint8_t kind);

// A sensible map zoom to show a result of this kind at (a city wants to be seen whole, a spring
// wants to be close).
uint8_t placenames_kind_zoom(uint8_t kind);

// Is there any place-name data on the card at all? One cheap check, never cached (the card mounts
// late and a failed early read must not stick - see the cached-failed-read lesson).
bool placenames_on_card(void);

// Hand back the ~260KB of PSRAM the search keeps while Maps is open (the top of cities.tnm).
void placenames_release(void);
