#include "configuration.h"

#include "TDeckCoverage.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "graphics/common/SdCard.h"

// -----------------------------------------------------------------------------------------
// MESH COVERAGE MAPPER
//
// Jake, 2026-09-22, picking from a shortlist. Worth building because he runs two solar relay
// nodes and currently has no way to know whether they are doing anything, or where a third one
// should go. Every received packet already carries RSSI and SNR; the GPS already knows where we
// are. Putting the two together turns an ordinary walk into a survey.
//
// ⭐ IT BINS INTO A GRID RATHER THAN LOGGING EVERY PACKET, and that is the whole design.
// A raw log grows without limit, is mostly duplicates (248 nodes chattering while you stand
// still), and still needs binning before it can be drawn. A grid is bounded, is already the
// thing the map wants to draw, and answers the actual question - "does the mesh reach here" -
// rather than "what happened at 14:32:07".
//
// ⛔ THE SAMPLE FUNCTION RUNS ON THE "tft" TASK, which holds spiLock for as long as it runs.
// It does a hash insert and nothing else: no SD, no logging, no allocation. All the SD work
// happens in tdeck_coverage_service() on the main loop. This is the same rule the rest of the
// device lives by, and the same one three separate bugs came from ignoring in one day.
// -----------------------------------------------------------------------------------------

extern "C" bool tdeck_gps_position(int32_t *lat, int32_t *lon);

// ⭐ CELL SIZE IS A POWER OF TWO so binning is a shift, not a divide - this runs per packet on
// the UI thread. 4096 units of 1e-7 degrees = 0.0004096 deg. In latitude that is ~45 m
// everywhere. In LONGITUDE it shrinks with the cosine of latitude: ~32 m at Jake's ~45 deg N.
// Cells are therefore slightly taller than they are wide, which is correct for a lat/lon grid
// and is exactly how the map draws them anyway.
static const int32_t kCellShift = 12; // 1<<12 = 4096
static const int kMaxCells = 6000;    // ~96KB in PSRAM; 6000 x 45m cells is a large survey

struct Cell {
    int32_t latBin, lonBin; // cell index, not degrees
    int16_t bestSnrQ;       // best SNR seen, quarter-dB
    int16_t bestRssi;       // best (least negative) RSSI, dBm
    uint16_t n;             // packets landed here
    uint8_t used;
};

static Cell *s_cells = nullptr;
static int s_count = 0;
static bool s_on = false;
static volatile bool s_dirty = false;  // something changed since the last save
static uint32_t s_nextSave = 0;
static bool s_loaded = false;

static const char *kFile = "/coverage.csv";

// Open addressing with linear probing. No deletion, so no tombstones needed. Chosen over a
// sorted array because this is hit from the packet path and must be O(1) with no shuffling.
static inline uint32_t cellHash(int32_t a, int32_t b)
{
    uint32_t h = (uint32_t)a * 0x9E3779B1u ^ (uint32_t)b * 0x85EBCA77u;
    h ^= h >> 15;
    return h;
}

static bool ensureTable(void)
{
    if (s_cells)
        return true;
    // PSRAM: this is a big, cold, non-urgent buffer and internal RAM is the scarce thing on
    // this device (largest free block measured at 20KB). Nothing here is touched from an ISR.
    s_cells = (Cell *)heap_caps_calloc(kMaxCells, sizeof(Cell), MALLOC_CAP_SPIRAM);
    if (!s_cells)
        s_cells = (Cell *)calloc(kMaxCells, sizeof(Cell)); // tiny boards / no PSRAM
    return s_cells != nullptr;
}

static Cell *findOrAdd(int32_t latBin, int32_t lonBin, bool add)
{
    if (!s_cells)
        return nullptr;
    uint32_t i = cellHash(latBin, lonBin) % (uint32_t)kMaxCells;
    for (int probe = 0; probe < kMaxCells; probe++) {
        Cell &c = s_cells[i];
        if (!c.used) {
            if (!add || s_count >= kMaxCells - 1)
                return nullptr;
            c.used = 1;
            c.latBin = latBin;
            c.lonBin = lonBin;
            c.bestSnrQ = -32768;
            c.bestRssi = -32768;
            c.n = 0;
            s_count++;
            return &c;
        }
        if (c.latBin == latBin && c.lonBin == lonBin)
            return &c;
        i++;
        if (i >= (uint32_t)kMaxCells)
            i = 0;
    }
    return nullptr;
}

extern "C" bool tdeck_coverage_enabled(void)
{
    return s_on;
}

extern "C" void tdeck_coverage_set_enabled(bool on)
{
    if (on && !s_loaded)
        tdeck_coverage_load();
    s_on = on;
    LOG_INFO("coverage: recording %s (%d cells)", on ? "ON" : "off", s_count);
}

extern "C" void tdeck_coverage_sample(uint32_t fromNode, int32_t rssi, float snr)
{
    (void)fromNode; // the grid is about PLACES, not who was talking
    if (!s_on || !ensureTable())
        return;
    // A packet with no signal figures is a local or MQTT-delivered one, not something the radio
    // heard. Recording those would paint coverage where there is none - the worst possible
    // failure for a tool whose whole job is to be trusted about where the mesh reaches.
    if (rssi == 0 && snr == 0.0f)
        return;
    int32_t lat = 0, lon = 0;
    if (!tdeck_gps_position(&lat, &lon) || (lat == 0 && lon == 0))
        return; // no fix, no idea where we are, so the sample is worthless

    Cell *c = findOrAdd(lat >> kCellShift, lon >> kCellShift, true);
    if (!c)
        return;
    int16_t q = (int16_t)lroundf(snr * 4.0f);
    if (q > c->bestSnrQ)
        c->bestSnrQ = q;
    if ((int16_t)rssi > c->bestRssi)
        c->bestRssi = (int16_t)rssi;
    if (c->n < 65535)
        c->n++;
    s_dirty = true;
}

// ⭐ SYNTHETIC CELLS, FOR PROVING THE OVERLAY DRAWS.
//
// The heatmap had never once been looked at on a screen, because looking at it requires a survey
// and a survey requires driving around - so the first person to find out whether it worked would
// have been Jake, after a wasted trip. This injects a known pattern at a known place so the
// drawing can be checked in ten seconds at a desk.
//
// ⛔ IT SETS s_synthetic, WHICH BLOCKS EVERY SAVE. Made-up coverage reaching /coverage.csv would
// be the single worst bug this feature could have - a map that confidently shows reception where
// there is none. A flag that has to be cleared is a guarantee; "remember to clear it afterwards"
// is not. tdeck_coverage_clear() is the only way out.
static bool s_synthetic = false;

extern "C" bool tdeck_coverage_is_synthetic(void)
{
    return s_synthetic;
}

extern "C" void tdeck_coverage_inject(int32_t lat, int32_t lon, int snrQ, int rssi)
{
    if (!ensureTable())
        return;
    s_synthetic = true;
    Cell *c = findOrAdd(lat >> kCellShift, lon >> kCellShift, true);
    if (!c)
        return;
    c->bestSnrQ = (int16_t)snrQ;
    c->bestRssi = (int16_t)rssi;
    if (c->n < 65535)
        c->n++;
    // deliberately NOT s_dirty - see above
}

extern "C" int tdeck_coverage_count(void)
{
    return s_count;
}

extern "C" bool tdeck_coverage_cell(int i, int32_t *lat, int32_t *lon, int *snrQ, int *rssi, int *n)
{
    if (!s_cells || i < 0)
        return false;
    // Walk the sparse table to the i-th occupied slot. The caller iterates 0..count-1, so this
    // is O(table) per full pass - fine at 6000 slots and once per map redraw, and it keeps the
    // insert path (which is on the UI thread) free of any bookkeeping.
    int seen = 0;
    for (int k = 0; k < kMaxCells; k++) {
        if (!s_cells[k].used)
            continue;
        if (seen++ != i)
            continue;
        const Cell &c = s_cells[k];
        // Centre of the cell, not its corner: the map draws a square around this point.
        const int32_t half = (int32_t)1 << (kCellShift - 1);
        if (lat)
            *lat = (c.latBin << kCellShift) + half;
        if (lon)
            *lon = (c.lonBin << kCellShift) + half;
        if (snrQ)
            *snrQ = c.bestSnrQ;
        if (rssi)
            *rssi = c.bestRssi;
        if (n)
            *n = c.n;
        return true;
    }
    return false;
}

extern "C" void tdeck_coverage_clear(void)
{
    if (s_cells)
        memset(s_cells, 0, (size_t)kMaxCells * sizeof(Cell));
    s_count = 0;
    s_dirty = false;
    s_synthetic = false; // the only way back to recording real coverage
    SDFs.remove(kFile);
    LOG_INFO("coverage: cleared");
}

extern "C" void tdeck_coverage_load(void)
{
    s_loaded = true;
    if (!ensureTable())
        return;
    FsFile f = SDFs.open(kFile, O_RDONLY);
    if (!f)
        return;
    char line[96];
    int got = 0;
    // Skip the header row.
    f.fgets(line, sizeof(line));
    while (f.fgets(line, sizeof(line)) > 0 && s_count < kMaxCells - 1) {
        int32_t lat = 0, lon = 0;
        int q = 0, r = 0, n = 0;
        if (sscanf(line, "%ld,%ld,%d,%d,%d", (long *)&lat, (long *)&lon, &q, &r, &n) != 5)
            continue;
        Cell *c = findOrAdd(lat >> kCellShift, lon >> kCellShift, true);
        if (!c)
            break;
        c->bestSnrQ = (int16_t)q;
        c->bestRssi = (int16_t)r;
        c->n = (uint16_t)n;
        got++;
    }
    f.close();
    LOG_INFO("coverage: loaded %d cells from %s", got, kFile);
}

extern "C" void tdeck_coverage_service(void)
{
    if (!s_dirty || !s_cells)
        return;
    if (s_synthetic) {
        // Belt and braces. inject() never sets s_dirty, but a real packet arriving while test
        // cells are in the table would - and then the save would write both out together.
        s_dirty = false;
        return;
    }
    // ⚠️ RATE LIMITED HARD. Rewriting the whole grid is a multi-KB SD write, and the SD shares
    // its SPI bus with the display and the LoRa radio. Every 30s while actually moving is
    // plenty: the grid is in RAM and the only thing a save protects against is a flat battery.
    uint32_t now = millis();
    if (s_nextSave && (int32_t)(now - s_nextSave) < 0)
        return;
    s_nextSave = now + 30000;
    s_dirty = false;

    // Whole-file rewrite rather than append: cells are UPDATED in place (a better SNR replaces
    // a worse one), so appending would grow a file full of superseded rows that the loader
    // would then have to de-duplicate. The grid is the truth; the file is just a copy of it.
    FsFile f = SDFs.open(kFile, O_WRONLY | O_CREAT | O_TRUNC);
    if (!f) {
        LOG_INFO("coverage: could not write %s", kFile);
        return;
    }
    f.println("lat_e7,lon_e7,best_snr_quarter_db,best_rssi_dbm,packets");
    char line[96];
    int wrote = 0;
    const int32_t half = (int32_t)1 << (kCellShift - 1);
    for (int k = 0; k < kMaxCells; k++) {
        if (!s_cells[k].used)
            continue;
        const Cell &c = s_cells[k];
        snprintf(line, sizeof(line), "%ld,%ld,%d,%d,%u", (long)((c.latBin << kCellShift) + half),
                 (long)((c.lonBin << kCellShift) + half), (int)c.bestSnrQ, (int)c.bestRssi, (unsigned)c.n);
        f.println(line);
        wrote++;
    }
    f.close();
    LOG_INFO("coverage: saved %d cells", wrote);
}
