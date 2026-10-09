#pragma once

#include "graphics/map/GeoPoint.h"
#include "graphics/map/MapTile.h"
#include "graphics/map/TileService.h"
#include "lvgl.h"
#include "util/PsramAlloc.h"

#include <memory>
#include <unordered_map>

/**
 * Size independent map panel for x/y/z raster tiles
 * Draws the tiles and objects to the provided lvgl panel.
 * Stores three positions:
 *   - home
 *   - GPS (current)
 *   - scrolled (manual)
 */
class MapPanel
{
  public:
    MapPanel(lv_obj_t *panel, ITileService *s = nullptr);

    // draw callback for map objects: img, x, y, zoom
    using DrawCallback = std::function<void(uint32_t, uint16_t, uint16_t, uint8_t)>;

    // replace service for loading tiles
    void setTileService(ITileService *s);
    void setBackupService(ITileService *s);
    // zooming
    void setZoom(uint8_t zoom);
    // follow GPS
    void setLocked(bool lock);
    // reset panel size to actual dimensions
    void updateDimensions(void);

    // positioning
    // set new home position according current
    void setHomePosition(void);
    void getHomeLocation(float &lat, float &lon) const;
    // set new home position
    void setHomeLocation(float lat, float lon);
    void setScrolledPosition(float lat, float lon);
    void setGpsPosition(float lat, float lon);
    bool scroll(int16_t deltaX, int16_t deltaY, uint16_t fraction = 3); // -1, 0, +1, 1/3
    bool scrollBy(int16_t scrollX, int16_t scrollY);                    // exact pixels, for drag-to-pan
    void moveHome(bool zoomDefault = true);
    void moveCurrent(void);
    // convert a panel pixel (x,y) to a geographic lat/lon (inverse of the object placement)
    void screenToGeo(int16_t x, int16_t y, float &lat, float &lon);
    // The inverse: where on the panel does this coordinate land? Returns false when it falls
    // outside the visible panel, so a caller drawing an overlay can skip it cheaply.
    // Deliberately does NOT consult the tile cache - it works off the same centre point
    // screenToGeo uses, so it is correct even for a coordinate whose tile has not loaded.
    bool geoToScreen(float lat, float lon, int16_t &x, int16_t &y);
    // current map center + last known GPS location
    void getCenter(float &lat, float &lon) const { lat = scrolled.latitude; lon = scrolled.longitude; }
    void getGpsLocation(float &lat, float &lon) const { lat = current.latitude; lon = current.longitude; }
    // placing objects
    void add(uint32_t id, float lat, float lon, DrawCallback drawCB);
    void update(uint32_t id, float lat, float lon);
    void update(uint32_t id, bool filtered);
    void remove(uint32_t id);
    uint32_t getObjectsOnMap(void) { return objectsOnMap; }
    // images
    void setHomeLocationImage(lv_obj_t *img);
    void setGpsPositionImage(lv_obj_t *img);
    void setNoTileImage(const lv_image_dsc_t *img_src);
    void forceRedraw(bool onlyObjects = false);
    bool redrawComplete(void) { return redrawCompleted; }
    // Drop the cached map tiles to reclaim RAM while this map isn't on screen.
    // Rebuilds automatically on the next redraw (same reset the redraw path uses).
    void releaseTiles(void);
    // for debugging
    void printTiles(void);
    // must be called for incremental drawing of all changes
    void task_handler(void);
    ~MapPanel(void);

  protected:
    // One per pin and per mesh node on the map, allocated with `new` - so in PSRAM, not the
    // internal heap (see util/PsramAlloc.h; ~245 of these was a large part of 27KB).
    struct MapObject {
        uint32_t id;
        GeoPoint point;
        DrawCallback draw;
        TUI_PSRAM_NEW_DELETE
    };

    void center(void);
    void redraw(void);
    void drawLocation(void);
    void drawObjects(void);
    void drawObject(MapObject &obj, bool count = false);

    bool needsRedraw = false;
    bool redrawCompleted = true;
    bool locked = false; // map follows GPS location

    // Incremental-redraw progress + failed-tile retry schedule. These were
    // function-local STATICS in redraw(), shared by every MapPanel instance —
    // with two panels alive (mesh map + Maps app, often showing the same tile
    // coordinates) they corrupted each other's progress and retry bookkeeping.
    int16_t redrawX = INT16_MAX;
    int16_t redrawY = INT16_MAX;
    PsramUnorderedMap<uint32_t, uint32_t> failedTilesRetryAt; // tile hash -> next retry (lv_tick ms)

    int16_t widthPixel;  // visible panel width
    int16_t heightPixel; // visible panel height
    int16_t xOffset;     // pixel offset x panel to upper left corner of (fully visible) upper left tile
    int16_t yOffset;     // pixel offset y panel to upper left corner of (fully visible) upper left tile
    uint32_t xStart;     // xTile number of (partly covered) upper left tile
    uint32_t yStart;     // yTile number of (partly covered) upper left tile
    uint8_t tilesX;      // number of (partly) visible tiles horizontal
    uint8_t tilesY;      // number of (partly) visible tiles vertical

    GeoPoint home;     // home location
    GeoPoint current;  // current (GPS) location
    GeoPoint scrolled; // current scrolled location, lat/lon always centered to the panel

    lv_obj_t *panel;                   // lvgl parent panel object
    lv_obj_t *homeLocationImage;       // lvgl image of home position
    lv_obj_t *gpsPositionImage;        // lvgl image of actual position
    const lv_image_dsc_t *noTileImage; // lvgl image src for displaying "no tile"
    TileService *service;              // tile service provider
    uint32_t objectsOnMap;             // num of visible objcts on map
    PsramUnorderedMap<uint32_t, std::unique_ptr<MapTile>> tiles;
    PsramUnorderedMap<uintptr_t, std::unique_ptr<MapObject>> mapObjects;
};
