/* Fills polygons exactly the way the SH2s do: the master turns each polygon into two
   vertex chains clipped to the 3D view (code overlay B), the slave walks them line by
   line with a fixed-point DDA (code overlay C). Same integer arithmetic, same pixels.
   Used to check the HLE against the SH2s' frame buffer. Rules: docs/scene-data*. */
#ifndef VRDE_SH2RASTER_H
#define VRDE_SH2RASTER_H

#include <stdint.h>

#include "hle.h"

#define SH2_SCREEN_WIDTH  320
#define SH2_SCREEN_HEIGHT 224

/* Draws one polygon into a SH2_SCREEN_WIDTH x SH2_SCREEN_HEIGHT image of palette
   indices (rows as in the 32X frame buffer), clipped to the lines view_top..view_bottom.
   Polygons only exact vertices draw are left out. */
void sh2_draw_polygon(uint8_t *image, const hle_polygon_t *polygon, int view_top, int view_bottom);

#endif
