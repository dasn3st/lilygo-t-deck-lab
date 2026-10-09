#if defined(HAS_SDCARD) && not defined(HAS_SD_MMC) && not defined(ARCH_PORTDUINO)

#include "lvgl.h"

#include "graphics/common/SdCard.h"
#include "graphics/map/MapTileSettings.h"
#include "graphics/map/SdFatService.h"
#include "util/ILog.h"
#include <lgfx/utility/lgfx_miniz.h>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <cstdint>

#define DRIVE_LETTER "S"

#include <cstring>

// from ConvertPNG.c — the same STBI decoders the online tile fetcher uses
extern "C" {
bool decodeImgGrey(const void *data, size_t size, lv_img_dsc_t **img);
bool decodeImgColor(const void *data, size_t size, lv_img_dsc_t **img);
char *stbi_zlib_decode_noheader_malloc(const char *buffer, int len, int *outlen);
void tdeck_stbi_arena_prepare(void);
}

#ifdef ARDUINO_ARCH_ESP32
#include <esp_heap_caps.h>
#endif

namespace {
// PMTiles v3 reader for the original offline map bundles.  The bundle stores the
// same lossless 256x256 PNGs as the old SD setup, but keeps the directory and tile
// data in one large file.  This avoids copying 337k tiny files to FAT32 and keeps
// the map quality identical to the source backup.
struct PmEntry {
    uint64_t tileId = 0;
    uint64_t offset = 0;
    uint64_t length = 0;
    uint64_t runLength = 0;
};

struct PmHeader {
    uint64_t rootOffset = 0;
    uint64_t rootLength = 0;
    uint64_t leafOffset = 0;
    uint64_t tileOffset = 0;
    uint8_t internalCompression = 0;
    uint8_t tileCompression = 0;
    uint8_t tileType = 0;
};

struct PmState {
    char style[48] = {};
    PmHeader header;
    std::vector<PmEntry> root;
    uint64_t leafCacheOffset = UINT64_MAX;
    uint64_t leafCacheLength = 0;
    std::vector<PmEntry> leaf;
};

PmState s_pm;

// Declared here because the PMTiles path shares the existing decoded-tile cache,
// whose implementation lives immediately below it in this file.
lv_img_dsc_t *tileCacheGet(const char *key);
void tileCachePut(const char *key, const lv_img_dsc_t *dsc);

uint64_t pmLe64(const uint8_t *p)
{
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i)
        value = (value << 8) | p[i];
    return value;
}

bool pmVarint(const uint8_t *data, size_t size, size_t &pos, uint64_t &value)
{
    value = 0;
    unsigned shift = 0;
    while (pos < size && shift <= 63) {
        const uint8_t byte = data[pos++];
        value |= uint64_t(byte & 0x7f) << shift;
        if (!(byte & 0x80))
            return true;
        shift += 7;
    }
    return false;
}

bool pmParseDirectory(const uint8_t *data, size_t size, std::vector<PmEntry> &out)
{
    size_t pos = 0;
    uint64_t count = 0;
    if (!pmVarint(data, size, pos, count) || count > 1000000)
        return false;

    out.assign((size_t)count, PmEntry{});
    uint64_t lastId = 0;
    for (size_t i = 0; i < (size_t)count; ++i) {
        uint64_t delta = 0;
        if (!pmVarint(data, size, pos, delta))
            return false;
        out[i].tileId = lastId + delta;
        lastId = out[i].tileId;
    }
    for (size_t i = 0; i < (size_t)count; ++i)
        if (!pmVarint(data, size, pos, out[i].runLength))
            return false;
    for (size_t i = 0; i < (size_t)count; ++i)
        if (!pmVarint(data, size, pos, out[i].length))
            return false;
    for (size_t i = 0; i < (size_t)count; ++i) {
        uint64_t encoded = 0;
        if (!pmVarint(data, size, pos, encoded))
            return false;
        if (i > 0 && encoded == 0)
            out[i].offset = out[i - 1].offset + out[i - 1].length;
        else if (encoded > 0)
            out[i].offset = encoded - 1;
        else
            return false;
    }
    return true;
}

uint8_t *pmGunzip(const uint8_t *data, size_t size, size_t &outSize)
{
    outSize = 0;
    if (!data || size < 18 || data[0] != 0x1f || data[1] != 0x8b || data[2] != 8)
        return nullptr;

    // PMTiles uses gzip for its internal directories.  Miniz handles the raw
    // deflate payload without consuming the PNG decoder's PSRAM arena.
    size_t pos = 10;
    const uint8_t flags = data[3];
    if (flags & 4) {
        if (pos + 2 > size)
            return nullptr;
        const size_t extra = data[pos] | (size_t(data[pos + 1]) << 8);
        pos += 2 + extra;
    }
    if (flags & 8) {
        while (pos < size && data[pos++]) {}
    }
    if (flags & 16) {
        while (pos < size && data[pos++]) {}
    }
    if (flags & 2)
        pos += 2;
    if (pos >= size || size - pos < 8)
        return nullptr;

    tdeck_stbi_arena_prepare();
    void *decoded = lgfx_tinfl_decompress_mem_to_heap(data + pos, size - pos - 8, &outSize, 0);
    if (!decoded || outSize == 0) {
        if (decoded)
            lgfx_mz_free(decoded);
        return nullptr;
    }
    return (uint8_t *)decoded;
}

bool pmRead(const char *path, uint64_t offset, uint64_t length, uint8_t *dest)
{
    if (!path || !dest || length == 0 || offset > UINT32_MAX || length > UINT32_MAX)
        return false;
    FsFile file = SDFs.open(path, O_RDONLY);
    if (!file)
        return false;
    const bool sought = file.seekSet((uint32_t)offset);
    const size_t got = sought ? (size_t)file.read(dest, (size_t)length) : 0;
    file.close();
    return sought && got == (size_t)length;
}

bool pmStylePath(const char *name, char *style, size_t styleSize, int &z, int &x, int &y)
{
    // SdFatService passes the mounted filesystem name ("S:/maps/...") here,
    // while the PMTiles files themselves are opened relative to SDFs as
    // "/maps/...".  Strip the drive prefix before parsing the tile path.
    if (!name)
        return false;
    const char *mapPath = name;
    if (mapPath[0] == DRIVE_LETTER[0] && mapPath[1] == ':')
        mapPath += 2;
    if (strncmp(mapPath, "/maps/", 6) != 0)
        return false;
    const char *styleStart = mapPath + 6;
    const char *styleSlash = strchr(styleStart, '/');
    if (!styleSlash || styleSlash == styleStart)
        return false;

    const size_t styleLen = (size_t)(styleSlash - styleStart);
    if (styleLen == 0 || styleLen >= styleSize)
        return false;
    memcpy(style, styleStart, styleLen);
    style[styleLen] = '\0';

    const char *zStart = styleSlash + 1;
    const char *zSlash = strchr(zStart, '/');
    if (!zSlash || zSlash == zStart)
        return false;

    const char *xStart = zSlash + 1;
    const char *xSlash = strchr(xStart, '/');
    if (!xSlash || xSlash == xStart)
        return false;

    const char *yStart = xSlash + 1;
    const char *dot = strchr(yStart, '.');
    if (!dot || dot == yStart)
        return false;

    z = atoi(zStart);
    x = atoi(xStart);
    y = atoi(yStart);
    if (z < 0 || z > 31 || x < 0 || y < 0 || x >= (1 << z) || y >= (1 << z))
        return false;
    return true;
}

void pmRotate(uint32_t n, uint32_t &x, uint32_t &y, uint32_t rx, uint32_t ry)
{
    if (ry == 0) {
        if (rx != 0) {
            x = n - 1 - x;
            y = n - 1 - y;
        }
        const uint32_t oldX = x;
        x = y;
        y = oldX;
    }
}

uint64_t pmTileId(int z, int x, int y)
{
    uint64_t id = ((uint64_t(1) << (z * 2)) - 1) / 3;
    uint32_t ux = (uint32_t)x;
    uint32_t uy = (uint32_t)y;
    for (int a = z - 1; a >= 0; --a) {
        const uint32_t s = uint32_t(1) << a;
        const uint32_t rx = s & ux;
        const uint32_t ry = s & uy;
        id += uint64_t((3 * rx) ^ ry) << a;
        pmRotate(s, ux, uy, rx, ry);
    }
    return id;
}

const PmEntry *pmFind(const std::vector<PmEntry> &entries, uint64_t tileId)
{
    if (entries.empty())
        return nullptr;
    size_t lo = 0, hi = entries.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (entries[mid].tileId <= tileId)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return nullptr;
    const PmEntry &candidate = entries[lo - 1];
    // A runLength of zero is a leaf-directory pointer.  In a parent directory
    // it covers the range until the next pointer, so predecessor matching is
    // required before descending into that leaf.
    if (candidate.tileId == tileId || candidate.runLength == 0 ||
        (tileId >= candidate.tileId && tileId - candidate.tileId < candidate.runLength))
        return &candidate;
    return nullptr;
}

bool pmOpenStyle(const char *style)
{
    if (strcmp(s_pm.style, style) == 0 && !s_pm.root.empty())
        return true;

    PmState fresh;
    snprintf(fresh.style, sizeof(fresh.style), "%s", style);
    char path[112];
    snprintf(path, sizeof(path), "/maps/%s/%s.pmtiles", style, style);

    uint8_t header[127];
    if (!pmRead(path, 0, sizeof(header), header) || memcmp(header, "PMTiles", 7) != 0 || header[7] != 3) {
        ILOG_DEBUG("PMTiles: header read failed %s", path);
        return false;
    }
    fresh.header.rootOffset = pmLe64(header + 8);
    fresh.header.rootLength = pmLe64(header + 16);
    fresh.header.leafOffset = pmLe64(header + 40);
    fresh.header.tileOffset = pmLe64(header + 56);
    fresh.header.internalCompression = header[97];
    fresh.header.tileCompression = header[98];
    fresh.header.tileType = header[99];
    if (fresh.header.internalCompression != 2 || fresh.header.tileCompression != 1 || fresh.header.tileType != 2 ||
        fresh.header.rootLength == 0 || fresh.header.rootLength > 1024 * 1024) {
        ILOG_DEBUG("PMTiles: unsupported header %s ic=%u tc=%u type=%u root=%lu", style,
                   fresh.header.internalCompression, fresh.header.tileCompression, fresh.header.tileType,
                   (unsigned long)fresh.header.rootLength);
        return false;
    }

    uint8_t *rootCompressed = (uint8_t *)lv_malloc((size_t)fresh.header.rootLength);
    if (!rootCompressed)
        return false;
    bool ok = pmRead(path, fresh.header.rootOffset, fresh.header.rootLength, rootCompressed);
    size_t rootSize = 0;
    uint8_t *rootDecoded = ok ? pmGunzip(rootCompressed, (size_t)fresh.header.rootLength, rootSize) : nullptr;
    lv_free(rootCompressed);
    if (!rootDecoded) {
        ILOG_DEBUG("PMTiles: root gzip failed %s", style);
        return false;
    }
    ok = pmParseDirectory(rootDecoded, rootSize, fresh.root);
    lgfx_mz_free(rootDecoded);
    if (!ok || fresh.root.empty()) {
        ILOG_DEBUG("PMTiles: root directory failed %s", style);
        return false;
    }

    s_pm = std::move(fresh);
    ILOG_DEBUG("PMTiles: opened %s (%u root entries)", style, (unsigned)s_pm.root.size());
    return true;
}

bool pmLoadTile(const char *name, lv_obj_t *img)
{
    char style[48];
    int z = 0, x = 0, y = 0;
    if (!pmStylePath(name, style, sizeof(style), z, x, y)) {
        ILOG_DEBUG("PMTiles: path parse failed %s", name ? name : "(null)");
        return false;
    }
    ILOG_DEBUG("PMTiles: tile path %s style=%s z=%d x=%d y=%d", name, style, z, x, y);
    if (!pmOpenStyle(style))
        return false;

    const uint64_t tileId = pmTileId(z, x, y);
    const PmEntry *entry = pmFind(s_pm.root, tileId);
    if (!entry) {
        ILOG_DEBUG("PMTiles: root miss %s z=%d x=%d y=%d id=%lu", style, z, x, y, (unsigned long)tileId);
        return false;
    }

    char path[112];
    snprintf(path, sizeof(path), "/maps/%s/%s.pmtiles", style, style);
    if (entry->runLength == 0) {
        if (entry->length == 0 || entry->length > 1024 * 1024 ||
            entry->offset > UINT32_MAX || s_pm.header.leafOffset + entry->offset > UINT32_MAX) {
            ILOG_DEBUG("PMTiles: bad leaf ref off=%lu len=%lu", (unsigned long)entry->offset,
                       (unsigned long)entry->length);
            return false;
        }
        if (s_pm.leafCacheOffset != entry->offset || s_pm.leafCacheLength != entry->length) {
            uint8_t *compressed = (uint8_t *)lv_malloc((size_t)entry->length);
            if (!compressed) {
                ILOG_DEBUG("PMTiles: leaf alloc failed len=%lu", (unsigned long)entry->length);
                return false;
            }
            const bool readOk = pmRead(path, s_pm.header.leafOffset + entry->offset, entry->length, compressed);
            size_t leafSize = 0;
            uint8_t *decoded = readOk ? pmGunzip(compressed, (size_t)entry->length, leafSize) : nullptr;
            lv_free(compressed);
            if (!decoded) {
                ILOG_DEBUG("PMTiles: leaf gzip failed off=%lu len=%lu", (unsigned long)entry->offset,
                           (unsigned long)entry->length);
                return false;
            }
            std::vector<PmEntry> parsed;
            const bool parseOk = pmParseDirectory(decoded, leafSize, parsed);
            lgfx_mz_free(decoded);
            if (!parseOk) {
                ILOG_DEBUG("PMTiles: leaf directory failed off=%lu len=%lu", (unsigned long)entry->offset,
                           (unsigned long)entry->length);
                return false;
            }
            s_pm.leaf = std::move(parsed);
            s_pm.leafCacheOffset = entry->offset;
            s_pm.leafCacheLength = entry->length;
            ILOG_DEBUG("PMTiles: leaf ready entries=%u", (unsigned)s_pm.leaf.size());
        }
        entry = pmFind(s_pm.leaf, tileId);
        if (!entry) {
            ILOG_DEBUG("PMTiles: leaf miss %s z=%d x=%d y=%d id=%lu", style, z, x, y,
                       (unsigned long)tileId);
            return false;
        }
    }

    if (entry->length == 0 || entry->length > 512 * 1024 ||
        s_pm.header.tileOffset + entry->offset > UINT32_MAX) {
        ILOG_DEBUG("PMTiles: bad tile ref off=%lu len=%lu", (unsigned long)entry->offset,
                   (unsigned long)entry->length);
        return false;
    }
    ILOG_DEBUG("PMTiles: tile hit off=%lu len=%lu", (unsigned long)entry->offset, (unsigned long)entry->length);
    lv_img_dsc_t *cached = tileCacheGet(name);
    if (cached) {
        lv_image_set_src(img, cached);
        return true;
    }
    uint8_t *raw = (uint8_t *)lv_malloc((size_t)entry->length);
    if (!raw) {
        ILOG_DEBUG("PMTiles: tile alloc failed len=%lu", (unsigned long)entry->length);
        return false;
    }
    ILOG_DEBUG("PMTiles: reading tile");
    const bool readOk = pmRead(path, s_pm.header.tileOffset + entry->offset, entry->length, raw);
    ILOG_DEBUG("PMTiles: tile read done ok=%d", readOk ? 1 : 0);
    lv_img_dsc_t *dsc = nullptr;
    ILOG_DEBUG("PMTiles: decoding tile");
    bool decodeOk = readOk && (MapTileSettings::color() ? decodeImgColor(raw, (size_t)entry->length, &dsc)
                                                         : decodeImgGrey(raw, (size_t)entry->length, &dsc));
    ILOG_DEBUG("PMTiles: tile decode done ok=%d", decodeOk ? 1 : 0);
    lv_free(raw);
    if (!decodeOk || !dsc) {
        ILOG_DEBUG("PMTiles: tile read/decode failed read=%d len=%lu", readOk ? 1 : 0,
                   (unsigned long)entry->length);
        return false;
    }
    tileCachePut(name, dsc);
    lv_image_set_src(img, dsc);
    return true;
}

// Decoded-tile cache for the STBI path. A JPEG decode costs ~100ms of UI time and
// panning reloads tiles constantly (MAP_FULL_REDRAW), which made JPEG maps crawl.
// Keep the last few tiles' decoded pixels in PSRAM; a repeat load hands out a fresh
// COPY (a few ms of memcpy) so ownership stays exactly like a fresh decode: MapTile
// frees its copy, the cache owns its own buffers outright, lifetimes never tangle.
struct TileCacheEntry {
    char key[112];
    uint8_t *px = nullptr;
    uint32_t size = 0;
    lv_image_header_t header;
    uint32_t stamp = 0;
};
// 12, not 18. Each slot is a decoded 256x256 tile - 128KB - so 18 slots reserve 2.3MB of a
// 2.4MB PSRAM pool, and the device's own low-water records show PSRAM getting down to 65KB free
// in map-heavy sessions. A cache that leaves no room for the thing it is caching for is not
// helping. 12 still holds a full screen (~9 visible) plus three, so panning back a step is still
// instant, while handing ~768KB back to everything else that needs PSRAM - not least the decode
// buffer each new tile has to allocate before it can be cached at all.
constexpr int kTileCacheSlots = 12;
TileCacheEntry s_tileCache[kTileCacheSlots];
uint32_t s_tileStamp = 0;

uint8_t *tileCacheAlloc(size_t sz)
{
#ifdef ARDUINO_ARCH_ESP32
    return (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
#else
    return (uint8_t *)lv_malloc(sz);
#endif
}

void tileCacheFree(uint8_t *p)
{
#ifdef ARDUINO_ARCH_ESP32
    heap_caps_free(p);
#else
    lv_free(p);
#endif
}

// Build a MapTile-ownable dsc (USER1 flag, lv_malloc'd) from cached pixels.
lv_img_dsc_t *tileCacheMakeDsc(const TileCacheEntry &e)
{
    uint8_t *data = (uint8_t *)lv_malloc(e.size);
    if (!data)
        return nullptr;
    memcpy(data, e.px, e.size);
    lv_img_dsc_t *dsc = (lv_img_dsc_t *)lv_malloc_zeroed(sizeof(lv_img_dsc_t));
    if (!dsc) {
        lv_free(data);
        return nullptr;
    }
    dsc->header = e.header;
    dsc->data = data;
    dsc->data_size = e.size;
    return dsc;
}

lv_img_dsc_t *tileCacheGet(const char *key)
{
    for (auto &e : s_tileCache) {
        if (e.px && strcmp(e.key, key) == 0) {
            e.stamp = ++s_tileStamp;
            return tileCacheMakeDsc(e);
        }
    }
    return nullptr;
}

void tileCachePut(const char *key, const lv_img_dsc_t *dsc)
{
    if (strlen(key) >= sizeof(s_tileCache[0].key))
        return;
    // ⛔ ONLY GROW WHILE THERE IS ROOM TO SPARE. Twelve full slots is 1.5MB, and with the decode
    // arena and anything else resident that took PSRAM to within a few KB of empty - and the next
    // ordinary allocation elsewhere (a node-DB save wants 80KB in one piece) failed and crashed the
    // device. The old /diaglog.txt CRASH records show exactly that: psram_low=64k. Below the
    // headroom mark the cache stops growing and recycles its least recently used slot instead.
    bool roomToGrow = true;
#ifdef ARDUINO_ARCH_ESP32
    roomToGrow = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > (768u * 1024u + dsc->data_size);
#endif
    TileCacheEntry *slot = nullptr;
    TileCacheEntry *lru = nullptr;
    for (auto &e : s_tileCache) {
        if (!e.px) {
            if (!slot && roomToGrow)
                slot = &e; // a free slot, and room to fill it
            continue;
        }
        if (!lru || e.stamp < lru->stamp)
            lru = &e;
    }
    if (!slot)
        slot = lru; // full, or tight: recycle the least recently used
    if (!slot)
        return; // nothing cached yet and no room to start - just do not cache this one
    if (slot->px && !roomToGrow) {
        // Free the old pixels FIRST so the new ones can take their place rather than stack on top.
        tileCacheFree(slot->px);
        slot->px = nullptr;
    }
    uint8_t *px = tileCacheAlloc(dsc->data_size);
    if (!px)
        return; // PSRAM tight — just skip caching this one
    if (slot->px)
        tileCacheFree(slot->px);
    memcpy(px, dsc->data, dsc->data_size);
    strcpy(slot->key, key);
    slot->px = px;
    slot->size = dsc->data_size;
    slot->header = dsc->header;
    slot->stamp = ++s_tileStamp;
}
} // namespace

// ⛔ HAND THE DECODED-TILE CACHE BACK WHEN MAPS CLOSES. Twelve 128KB slots is 1.5MB of PSRAM -
// measured 2026-09-29 as the whole drop from 2.5MB free to 1.0MB on opening Maps - and it was
// kept for the rest of the session. Chess then could not get the 1MB table it thinks with
// (largest free block 1,015,796 bytes), so its search silently failed and left the game with
// Black to move and nobody to move it. A cache is only worth what it saves, and nothing is
// panning a map that is not on screen; reopening costs one decode per visible tile.
extern "C" void tdeck_stbi_arena_release(void); // ConvertPNG.c - the 920KB decode arena

extern "C" void tdeck_tile_cache_clear(void)
{
    tdeck_stbi_arena_release(); // and the decoder's scratch arena, which is the other 920KB
    for (auto &e : s_tileCache) {
        if (e.px)
            tileCacheFree(e.px);
        e.px = nullptr;
        e.size = 0;
        e.key[0] = 0;
        e.stamp = 0;
    }
}

SdFatService::SdFatService() : ITileService(DRIVE_LETTER ":")
{
    static lv_fs_drv_t drv;
    lv_fs_drv_init(&drv);
    drv.letter = DRIVE_LETTER[0];
    drv.cache_size = MapTileSettings::getCacheSize();
    drv.ready_cb = nullptr;
    drv.open_cb = fs_open;
    drv.close_cb = fs_close;
    drv.read_cb = fs_read;
    drv.write_cb = fs_write;
    drv.seek_cb = fs_seek;
    drv.tell_cb = fs_tell;
    drv.dir_open_cb = fs_dir_open;
    drv.dir_read_cb = fs_dir_read;
    drv.dir_close_cb = fs_dir_close;
    lv_fs_drv_register(&drv);
}

SdFatService::~SdFatService()
{
    SDFs.end();
}

bool SdFatService::load(const char *name, void *img)
{
    // The original offline maps are PMTiles bundles.  Load those before the
    // ordinary LVGL file path; if a bundle is absent, retain the legacy PNG/JPEG
    // behavior for downloaded or manually unpacked maps.
    if (pmLoadTile(name, (lv_obj_t *)img))
        return true;

    // JPEG tiles (the on-device USGS download writes these): LVGL's built-in TJPGD
    // decoder draws them black on this device, so read the file ourselves and decode
    // through STBI — the exact path the online fetcher uses for the same tiles.
    // decodeImg* tags the dsc LV_IMAGE_FLAGS_USER1, so MapTile::removeImage frees it
    // like any fetched tile. PNG stays on LVGL's LODEPNG path, which works.
    const char *ext = strrchr(name, '.');
    if (ext && strcmp(ext, ".jpg") == 0) {
        lv_img_dsc_t *cached = tileCacheGet(name);
        if (cached) {
            lv_image_set_src((lv_obj_t *)img, cached);
            return true;
        }
        FsFile f = SDFs.open(name, O_RDONLY);
        if (!f)
            return false;
        size_t len = f.size();
        uint8_t *raw = (len && len < 512u * 1024u) ? (uint8_t *)lv_malloc(len) : nullptr;
        if (!raw) {
            f.close();
            return false;
        }
        bool ok = (size_t)f.read(raw, len) == len;
        f.close();
        lv_img_dsc_t *dsc = nullptr;
        if (ok)
            ok = MapTileSettings::color() ? decodeImgColor(raw, len, &dsc) : decodeImgGrey(raw, len, &dsc);
        lv_free(raw);
        if (!ok || !dsc) {
            ILOG_DEBUG("STBI decode failed for SD tile %s", name);
            return false;
        }
        tileCachePut(name, dsc);
        lv_image_set_src((lv_obj_t *)img, dsc);
        return true;
    }

    char buf[128] = DRIVE_LETTER ":";
    strcat(&buf[2], name);
    // ILOG_DEBUG("SdFatService::load(): %s", buf);
    lv_image_set_src((lv_obj_t *)img, buf);
    if (!lv_image_get_src((lv_obj_t *)img)) {
        ILOG_DEBUG("Failed to load tile %s from SD", buf);
        return false;
    }
    // ILOG_INFO("*** Tile %s loaded.", buf);
    return true;
}

bool SdFatService::save(const char *name, void *img, size_t len)
{
    ILOG_DEBUG("SdFatService::save(%s): %d", name, len);
    // create intermediate directories for path (e.g. /maps/atlas/12/2198/1341.png)
    std::string directory;
    std::string filename(name);
    const size_t last_slash_idx = filename.rfind('/');
    if (std::string::npos == last_slash_idx) {
        // something went wrong
        return false;
    }
    directory = filename.substr(0, last_slash_idx);
    SDFs.mkdir(directory.c_str());

    // write image
    FsFile file = SDFs.open(name, O_RDWR | O_CREAT);
    if (file) {
        size_t written = file.write(static_cast<uint8_t *>(img), len);
        file.close();
        return written == len;
    } else {
        ILOG_ERROR("failed to write %s", name);
    }
    return false;
}

void *SdFatService::fs_open(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    String s(path);
    SdFile *lf = new SdFile;
    lf->file = SDFs.open(path, mode == LV_FS_MODE_RD ? O_RDONLY : O_WRONLY); // NOTE: O_RDWR
    if (!lf->file) {
        // ILOG_DEBUG("FsSD.open() %s failed!", path);
        delete lf;
        return nullptr;
    } else {
        return static_cast<void *>(lf);
    }
}

lv_fs_res_t SdFatService::fs_close(lv_fs_drv_t *drv, void *file_p)
{
    // ILOG_DEBUG("FsSD.close()");
    SdFile *lf = static_cast<SdFile *>(file_p);
    lf->file.close();
    delete lf;
    return LV_FS_RES_OK;
}

lv_fs_res_t SdFatService::fs_read(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br)
{
    *br = static_cast<SdFile *>(file_p)->file.read((uint8_t *)buf, btr);
    // ILOG_DEBUG("FsSD.read(): %d/%d bytes", *br, btr);
    return (*br <= 0) ? LV_FS_RES_UNKNOWN : LV_FS_RES_OK;
}

lv_fs_res_t SdFatService::fs_write(lv_fs_drv_t *drv, void *file_p, const void *buf, uint32_t btw, uint32_t *bw)
{
    *bw = static_cast<SdFile *>(file_p)->file.write((uint8_t *)buf, btw);
    // ILOG_DEBUG("FsSD.write(): %d/btw bytes", *bw, btw);
    return (*bw <= 0) ? LV_FS_RES_UNKNOWN : LV_FS_RES_OK;
}

lv_fs_res_t SdFatService::fs_seek(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence)
{
    // ILOG_DEBUG("FsSD.seek(): pos %d", pos);
    if (whence == LV_FS_SEEK_SET) {
        return static_cast<SdFile *>(file_p)->file.seekSet(pos) ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
    } else if (whence == LV_FS_SEEK_END) {
        return static_cast<SdFile *>(file_p)->file.seekEnd() ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
    } else {
        return static_cast<SdFile *>(file_p)->file.seekCur(pos) ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
    }
}

lv_fs_res_t SdFatService::fs_tell(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
    *pos_p = static_cast<SdFile *>(file_p)->file.position();
    // ILOG_DEBUG("FsSD.tell(): pos %d", *pos_p);
    return (int32_t)(*pos_p) < 0 ? LV_FS_RES_UNKNOWN : LV_FS_RES_OK;
}

void *SdFatService::fs_dir_open(lv_fs_drv_t *drv, const char *path)
{
    return nullptr; // TODO
}

lv_fs_res_t SdFatService::fs_dir_read(lv_fs_drv_t *drv, void *rddir_p, char *fn, uint32_t fn_len)
{
    return LV_FS_RES_NOT_IMP; // TODO
}

lv_fs_res_t SdFatService::fs_dir_close(lv_fs_drv_t *drv, void *rddir_p)
{
    return LV_FS_RES_NOT_IMP; // TODO
}

#endif
