/* Comparing a picture of the HLE's 3D with the one the SH2s drew, pixel by pixel.
   Used by vrdetrace -h (the SH2s' own fill, src/sh2raster.c) and vrde -t (the GPU's).
   Needs hud.c to follow the master SH2's writes. */
#ifndef VRDE_HLECHECK_H
#define VRDE_HLECHECK_H

#include <stdint.h>

#include "hle.h"

/* Compares a 320 x 224 image of palette indices (0 = nothing drawn), the frame's 3D, with
   the 32X frame buffer on screen, in the frame's 3D view (rows 16-207 in a race). Left out: pixels that neither drew, and
   the pixels the front end takes from the frame buffer (hud_keep_framebuffer_pixel). Where the
   images agree on every other pixel, the HLE's 3D with the HUD laid over it is the SH2s'
   picture. */
void hlecheck_compare(const hle_frame_t *frame, const uint8_t *image, int *compared, int *same);

/* Whether a pixel takes part in the comparison, and the frame buffer's palette index */
int hlecheck_is_compared(const hle_frame_t *frame, const uint8_t *image, int x, int y, int *framebuffer_index);

#endif
