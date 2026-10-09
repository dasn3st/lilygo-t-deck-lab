#include "graphics/map/OSMTiles.h"
#include "lvgl.h"
#include "stdint.h"

/**
 * Map tile graphical element. Creates and loads a tile image using the OSMTile provider.
 */
class MapTile : public OSMTiles<lv_obj_t>::Tile
{
  public:
    MapTile(uint32_t xTile, uint32_t yTile);
    bool load(lv_obj_t *p, int16_t posx, int16_t posy, const lv_image_dsc_t *noTile);
    bool move(int16_t posx, int16_t posy);
    void unload(void);

    int16_t getX(void) const { return x; }
    int16_t getY(void) const { return y; }

    void removeImage(void);
    ~MapTile();

  protected:
    int16_t x;     // x-pos in parent panel
    int16_t y;     // y-pos in parent panel
    int16_t ozdx = 0; // overzoom: how far the blown-up parent is shifted off this tile's slot,
    int16_t ozdy = 0; // in panel pixels. 0 when the tile loaded normally. move() must reapply it.
    lv_obj_t *img; // lvgl tile image
    lv_obj_t *lbl; // debug label
};