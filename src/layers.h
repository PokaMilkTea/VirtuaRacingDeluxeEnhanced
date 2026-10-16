/* The picture's two layers, kept apart. PicoDrive draws the Mega Drive picture line by
   line, and then lays the 32X frame buffer over a run of lines at once (at the end of the
   visible lines, or earlier when the SH2s change the 32X's display settings mid-frame).
   We copy both layers at that moment, exactly as PicoDrive reads them, so the front end
   can composite the picture itself, and later put the HLE's 3D in place of the 32X layer. */
#ifndef VRDE_LAYERS_H
#define VRDE_LAYERS_H

#include <stdint.h>

/* Rows are those of core_screen (core.h): the visible picture starts at core_visible_top. */
#define LAYERS_WIDTH  320
#define LAYERS_HEIGHT 240

/* Plane B drawn wider than the screen, SKY_MARGIN columns more on each side: in a race
   it is the sky, which smooth motion moves sideways between the game's scroll steps, next
   to a picture up to 32:9 wide */
#define SKY_MARGIN 384
#define SKY_WIDTH  (LAYERS_WIDTH + 2 * SKY_MARGIN)

typedef struct {
	/* The Mega Drive picture as PicoDrive drew it (RGB565), and the CRAM index of each
	   pixel; where that index is the background colour (VDP register 7), the 32X shows. */
	uint16_t md_picture[LAYERS_HEIGHT][LAYERS_WIDTH];
	uint8_t md_index[LAYERS_HEIGHT][LAYERS_WIDTH];
	uint8_t md_background[LAYERS_HEIGHT];

	/* Plane B (RGB565, colours as PicoDrive's), drawn by us from the VDP's memory as the
	   frame starts: column SKY_MARGIN is the screen's left edge. Where plane B is clear,
	   the background colour. md_wide_scroll is plane B's horizontal scroll it used. */
	uint16_t md_wide[LAYERS_HEIGHT][SKY_WIDTH];
	int md_wide_scroll;
	/* The screen line plane B's top row is on, and how many lines plane B has: the lines
	   above show its bottom rows again, wrapped round (in a race, black) */
	int md_wide_first_line, md_wide_lines;

	/* Whether the 32X layer was laid over the row at all (not before the 32X starts, nor
	   while it is blanking). */
	uint8_t x32_shown[LAYERS_HEIGHT];

	/* The 32X layer: a palette index per pixel, and the palette as it was for each row
	   (BGR555, bit 15 = priority, already inverted when the 32X's PRI bit asks for it). */
	uint8_t x32_index[LAYERS_HEIGHT][LAYERS_WIDTH];
	uint16_t x32_palette[LAYERS_HEIGHT][256];

	/* 1 where a race picture's pixel must stay the frame buffer's own even when the HLE
	   draws the 3D: the HUD graphics, and lines blanked through the line table
	   (hud_keep_framebuffer_pixel) */
	uint8_t x32_keep[LAYERS_HEIGHT][LAYERS_WIDTH];

	/* In a race picture, which entry of the HUD list drew each pixel, plus 1 (hud_item);
	   0 for the rest */
	uint8_t x32_hud_item[LAYERS_HEIGHT][LAYERS_WIDTH];
	/* The types of the HUD list's entries that drew the frame buffer shown (hud_drawn_types) */
	uint32_t x32_hud_types;
} layers_t;

extern layers_t layers;

/* 1: a run-length line that runs past the end of its frame buffer reads on into the
   memory after it, as PicoDrive does (for the bit-for-bit checks); 0: the rest of the
   line gets index 0 */
extern int layers_read_past_buffer_end;

/* Start copying the layers; call after core_init. */
void layers_init(void);

/* Plane B's horizontal scroll now (whole-screen scroll; 10 bits) */
int layers_plane_b_scroll(void);

/* Call before each core_run_frame. */
void layers_frame_start(void);

/* Call after each core_run_frame: rows the 32X layer never reached keep the Mega Drive
   picture only. */
void layers_frame_end(void);

#endif
