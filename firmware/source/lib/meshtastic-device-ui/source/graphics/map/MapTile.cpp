#include "graphics/map/MapTile.h"
#include "graphics/map/MapTileSettings.h"
#include "graphics/map/TileService.h"
#include "lvgl.h"
#include "lvgl_private.h" // lv_image_decoder_dsc_t - to read the parent tile's pixels for overzoom
#include "util/ILog.h"

#include <assert.h>
#include <string.h>

LV_IMAGE_DECLARE(img_no_tile_image);

OSMTiles<lv_obj_t> *osm = nullptr;

// Drop the image's source, freeing it when the tile owns it (LV_IMAGE_FLAGS_USER1 - a decoded or
// cached tile handed over by the tile service). The object itself stays.
static void releaseSrc(lv_obj_t *img)
{
    const void *src = lv_image_get_src(img);
    lv_image_set_src(img, NULL);
    if (src && lv_image_src_get_type(src) == LV_IMAGE_SRC_VARIABLE) {
        const lv_image_dsc_t *d = (const lv_image_dsc_t *)src;
        if (d->header.magic == LV_IMAGE_HEADER_MAGIC && (d->header.flags & LV_IMAGE_FLAGS_USER1)) {
            lv_image_cache_drop(src); // the pointer is about to be reused - no stale cache entry
            if (d->data)
                lv_free((void *)d->data);
            lv_free((void *)d);
        }
    }
}

// ⭐ OVERZOOM, DONE ONCE. Cut this tile's share of the loaded parent out and enlarge it into an
// ordinary tile-sized image, nearest neighbour. Jake, 2026-09-29: "maps gets slow at max zoom with
// its doing its 'artificial zoom'". The first version handed LVGL the WHOLE parent, scaled by 2-8x,
// in an object up to 2048x2048 - and every visible slot did the same, so ~9-12 identical enlarged
// images were stacked on top of each other and all of them re-scaled in software on every frame
// of a pan. This costs one 128KB copy when the tile loads and then draws like any other tile.
//
// Returns nullptr for anything it does not handle (odd size, a format with a separate alpha plane,
// no memory); the caller then falls back to the old scaled path, which is slow but correct.
static lv_image_dsc_t *cropUpscale(const void *src, int subX, int subY, int factor, int tileSize)
{
    lv_image_decoder_dsc_t dd;
    memset(&dd, 0, sizeof(dd));
    if (lv_image_decoder_open(&dd, src, NULL) != LV_RESULT_OK)
        return nullptr;
    lv_image_dsc_t *out = nullptr;
    const lv_draw_buf_t *db = dd.decoded;
    if (db && db->data) {
        const lv_color_format_t cf = (lv_color_format_t)db->header.cf;
        const bool plainFormat = cf == LV_COLOR_FORMAT_RGB565 || cf == LV_COLOR_FORMAT_RGB888 ||
                                 cf == LV_COLOR_FORMAT_ARGB8888 || cf == LV_COLOR_FORMAT_XRGB8888 ||
                                 cf == LV_COLOR_FORMAT_L8;
        const uint32_t bpp = lv_color_format_get_size(cf);
        const int part = tileSize / factor; // source pixels this tile covers along each side
        if (plainFormat && bpp >= 1 && bpp <= 4 && part > 0 && (int)db->header.w == tileSize &&
            (int)db->header.h == tileSize) {
            const uint32_t stride = (uint32_t)tileSize * bpp;
            const uint32_t srcStride = db->header.stride ? db->header.stride : stride;
            uint8_t *data = (uint8_t *)lv_malloc(stride * tileSize);
            out = data ? (lv_image_dsc_t *)lv_malloc_zeroed(sizeof(lv_image_dsc_t)) : nullptr;
            if (!out) {
                if (data)
                    lv_free(data);
            } else {
                const int x0 = subX * part, y0 = subY * part;
                for (int y = 0; y < tileSize; y++) {
                    uint8_t *d = data + (uint32_t)y * stride;
                    if (y % factor) { // same source row as the one above: copy it whole
                        memcpy(d, d - stride, stride);
                        continue;
                    }
                    const uint8_t *row = db->data + (uint32_t)(y0 + y / factor) * srcStride + (uint32_t)x0 * bpp;
                    for (int x = 0; x < part; x++) {
                        for (int k = 0; k < factor; k++) {
                            memcpy(d, row + (uint32_t)x * bpp, bpp);
                            d += bpp;
                        }
                    }
                }
                out->header.magic = LV_IMAGE_HEADER_MAGIC;
                out->header.cf = cf;
                out->header.w = tileSize;
                out->header.h = tileSize;
                out->header.stride = stride;
                out->header.flags = LV_IMAGE_FLAGS_USER1; // ours to free - see releaseSrc()
                out->data = data;
                out->data_size = stride * tileSize;
            }
        }
    }
    lv_image_decoder_close(&dd);
    return out;
}

MapTile::MapTile(uint32_t xTile, uint32_t yTile)
    : OSMTiles<lv_obj_t>::Tile(xTile, yTile, MapTileSettings::getZoomLevel()), img(nullptr), lbl(nullptr)
{
    // singleton should be already created
    assert(osm != nullptr);
}

/**
 * load map tile to display position x/y
 */
bool MapTile::load(lv_obj_t *p, int16_t posx, int16_t posy, const lv_image_dsc_t *img_src)
{
    x = posx;
    y = posy;
    ozdx = ozdy = 0; // a fresh load is not overzoomed until the block below says so
    if (!p)
        return false;
    removeImage();
    img = lv_image_create(p);
    lv_obj_set_pos(img, posx, posy);
    lv_obj_set_style_opa(img, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_size(img, MapTileSettings::getTileSize(), MapTileSettings::getTileSize());
    if (MapTileSettings::getDebug()) {
        lv_obj_set_style_border_width(img, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lbl = lv_label_create(img);
        lv_obj_set_pos(lbl, 0, 0);
        lv_obj_set_size(lbl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xff101010), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_text_fmt(lbl, "(%d/%d/%d) -> %d,%d", MapTileSettings::getZoomLevel(), xTile, yTile, posx, posy);
    }

    bool result = false;
#if LV_USE_FS_ARDUINO_SD
    // use lvgl built-in img loader
    char fname[128];
    fname[0] = LV_FS_ARDUINO_SD_LETTER;
    sprintf(&fname[1], ":%s/%s%d/%d/%d.%s", MapTileSettings::getPrefix(), MapTileSettings::getTileStyle(), zoomLevel, xTile,
            yTile, MapTileSettings::getTileFormat());
    ILOG_DEBUG("SD file: %s", fname);
    lv_image_set_src(img, fname);
    if (lv_image_get_src((lv_obj_t *)img)) {
        result = true;
    }
#endif
    // use configured TileService
    if (!result) {
        result = osm->load(*this, img);
    }

    // ⭐ OVERZOOM. Jake: "when you have no more map tiles at that zone. Could it like zoom in on
    // the actual map tile from the previous? It would be blurry but at least a bit better than
    // nothing right?" - yes, and it is what every map renderer does. USGS stops around z16 and
    // past that this drew a grey placeholder, which is worse than a blurry map: blurry still
    // shows the shape of the valley and where the track runs.
    //
    // ⛔ THIS MUST GO THROUGH osm->load(), NOT the lv_fs path above it. LV_USE_FS_ARDUINO_SD is
    // 0 in our lv_conf.h, so that block is dead code - the first version of this feature copied
    // its #if and was therefore never compiled in at all. Jake: "the extra zoom thing on the
    // maps isnt working. it just shows the 'no tile' placeholder still."
    //
    // Tile (z,x,y) lives inside (z-up, x>>up, y>>up). Rather than offset the source - which is
    // not what lv_image_set_offset_* does; it moves an unscaled rect in WIDGET pixels, before
    // lv_draw_image applies the scale - grow the object to the whole magnified parent and slide
    // it so our quadrant lands on our slot. pivot (0,0) keeps source (0,0) at the object's
    // top-left so it magnifies right and down.
    //
    // ⭐ THE OVERLAP IS HARMLESS, and that is the trick. This object now covers its siblings'
    // slots, but every child of the same parent computes (pos - sub*tileSize) from a pos that
    // differs by exactly tileSize per unit of sub, so they all land on the SAME rect and draw
    // the SAME magnified parent. Siblings paint identical pixels. No clipping container needed.
    if (!result) {
        const int tileSize = MapTileSettings::getTileSize();
        constexpr int kMaxOverzoom = 3; // 2x, 4x, 8x - past that it tells you nothing
        for (int up = 1; up <= kMaxOverzoom && !result; up++) {
            if ((int)zoomLevel - up < 0)
                break;
            OSMTiles<lv_obj_t>::Tile parent((uint32_t)(xTile >> up), (uint32_t)(yTile >> up), (uint8_t)(zoomLevel - up));
            if (!osm->load(parent, img))
                continue; // that ancestor is missing too - try one further up
            const int factor = 1 << up;
            const int subX = (int)(xTile & (uint32_t)(factor - 1));
            const int subY = (int)(yTile & (uint32_t)(factor - 1));
            // The fast path: an ordinary tile holding just our enlarged piece (see cropUpscale).
            {
                const void *psrc = lv_image_get_src(img);
                lv_image_dsc_t *piece = psrc ? cropUpscale(psrc, subX, subY, factor, tileSize) : nullptr;
                if (piece) {
                    releaseSrc(img);
                    lv_image_set_src(img, piece);
                    lv_obj_set_style_opa(img, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
                    ILOG_DEBUG("overzoom %d/%d/%d cut from parent %d/%d/%d (%dx)", zoomLevel, xTile, yTile,
                               (int)zoomLevel - up, xTile >> up, yTile >> up, factor);
                    result = true;
                    break;
                }
            }
            // Fallback: the whole parent, scaled at draw time. Correct, and slow.
            ozdx = (int16_t)(-subX * tileSize);
            ozdy = (int16_t)(-subY * tileSize);
            lv_obj_set_size(img, tileSize * factor, tileSize * factor);
            lv_obj_set_pos(img, x + ozdx, y + ozdy);
            lv_image_set_inner_align(img, LV_IMAGE_ALIGN_TOP_LEFT);
            lv_image_set_pivot(img, 0, 0);
            lv_image_set_scale(img, LV_SCALE_NONE * factor);
            lv_image_set_antialias(img, false); // a blown-up tile: nearest-neighbour is honest and cheap
            lv_obj_set_style_opa(img, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
            ILOG_DEBUG("overzoom %d/%d/%d from parent %d/%d/%d (%dx)", zoomLevel, xTile, yTile, (int)zoomLevel - up,
                       xTile >> up, yTile >> up, factor);
            result = true;
        }
    }

    if (!result) {
        {
            if (img_src) {
                // ILOG_DEBUG("set no-tile-image (%d/%d/%d)", MapTileSettings::getZoomLevel(), xTile, yTile);
                lv_image_set_src((lv_obj_t *)img, img_src);
                lv_obj_set_style_opa(img, 100, LV_PART_MAIN | LV_STATE_DEFAULT);
                if (!MapTileSettings::getDebug()) {
                    lv_obj_t *lbl = lv_label_create(img);
                    lv_obj_set_pos(lbl, 0, 50);
                    lv_obj_set_align(lbl, LV_ALIGN_CENTER);
                    lv_obj_set_size(lbl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
                    lv_obj_set_style_text_color(lbl, lv_color_hex(0xff505050), LV_PART_MAIN | LV_STATE_DEFAULT);
                    lv_label_set_text_fmt(lbl, "(%d/%d/%d)", MapTileSettings::getZoomLevel(), xTile, yTile);
                }
            }
        }
    }
    return result;
}

bool MapTile::move(int16_t posx, int16_t posy)
{
    x += posx;
    y += posy;
    // ozdx/ozdy are 0 for a normal tile. For an overzoomed one they are where the magnified
    // parent has to sit for OUR quadrant to land on OUR slot - drop them and panning tears the
    // alignment apart one step at a time.
    if (img)
        lv_obj_set_pos(img, x + ozdx, y + ozdy);
    if (MapTileSettings::getDebug()) {
        lv_label_set_text_fmt(lbl, "(%d/%d/%d) -> %d,%d", MapTileSettings::getZoomLevel(), xTile, yTile, x, y);
    }
    return true;
}

void MapTile::removeImage(void)
{
    if (!img) {
        return;
    }

    releaseSrc(img); // clears the source first so LVGL stops referencing it, then frees it if ours
    lv_obj_delete(img);
    img = nullptr;
}

MapTile::~MapTile()
{
    // ILOG_DEBUG("MapTile::~MapTile %d/%d/%d", zoomLevel, xTile, yTile);
    removeImage();
}
