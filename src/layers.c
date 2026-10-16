/* The picture's two layers, kept apart (see layers.h). Everything here follows the code
   that lays the 32X layer over the Mega Drive picture in PicoDrive (pico/32x/draw.c,
   do_loop_pp and do_loop_rl), reading the same memory at the same moment. */
#include <stdio.h>
#include <string.h>

#include "pico/pico_int.h"
#include "core.h"
#include "hud.h"
#include "layers.h"

layers_t layers;
int layers_read_past_buffer_end;

/* Words in one 32X frame buffer */
#define FRAMEBUFFER_WORDS 0x10000

static uint8_t row_captured[LAYERS_HEIGHT];

/* 32X display modes (32X VDP register 0, bits 0-1) */
enum { X32_BLANK, X32_PACKED_PIXEL, X32_DIRECT_COLOUR, X32_RUN_LENGTH };

/* A byte of a 32X frame buffer, in the SH2's (big-endian) byte order. PicoDrive keeps
   the buffer as 16-bit words in host order. */
static uint8_t framebuffer_byte(const uint16_t *dram, uint32_t byte_address)
{
	uint16_t word = dram[byte_address / 2];
	int is_high_byte = (byte_address % 2 == 0);
	return is_high_byte ? (word >> 8) : (word & 0xFF);
}

/* With the 32X's SFT bit set, a line moves one pixel left, unless its start's low byte
   is FF (that is how PicoDrive does it). */
static int line_shift(uint16_t line_start)
{
	int shift_bit = Pico32x.vdp_regs[2 / 2] & P32XV_SFT;
	int low_byte_is_ff = ((line_start + 1) & 0xFF) == 0;
	return shift_bit && !low_byte_is_ff;
}

/* Packed pixel: a byte per pixel. */
static void decode_packed_line(const uint16_t *dram, uint16_t line_start, uint8_t *out)
{
	uint32_t first_byte = line_start * 2 + line_shift(line_start);
	for (int x = 0; x < LAYERS_WIDTH; x++)
		out[x] = framebuffer_byte(dram, first_byte + x);
}

/* Run length: words of (run length - 1) << 8 | palette index. With SFT the first run
   is one pixel shorter. The SEGA logo gives each line a 256-word slot and leaves blank
   lines as zeros (one-pixel black runs), so a blank line reads on into the next slot;
   the last line runs past the buffer's end for its last 64 pixels. PicoDrive reads on
   into whatever follows in its own memory: for buffer 0 the other buffer's line table
   (black runs), for buffer 1 the 68000 vector table PicoDrive generates, coloured noise
   on the right; unless layers_read_past_buffer_end, those pixels get index 0, as the
   rest of that line. */
static void decode_run_length_line(const uint16_t *dram, uint16_t line_start, uint8_t *out)
{
	int pending = -line_shift(line_start);       /* pixels left of the current run */
	int x = 0;
	uint32_t word_address = line_start;
	while (x < LAYERS_WIDTH) {
		if (word_address >= FRAMEBUFFER_WORDS && !layers_read_past_buffer_end) {
			memset(&out[x], 0, LAYERS_WIDTH - x);
			break;
		}
		uint16_t run = dram[word_address];
		word_address++;
		pending += (run >> 8) + 1;
		while (pending > 0 && x < LAYERS_WIDTH) {
			out[x] = run & 0xFF;
			x++;
			pending--;
		}
	}
}

static void copy_32x_palette(uint16_t *out)
{
	int invert_priority = (Pico32x.vdp_regs[0] & P32XV_PRI) != 0;
	for (int i = 0; i < 256; i++) {
		uint16_t colour = Pico32xMem->pal[i];
		if (invert_priority)
			colour ^= 0x8000;
		out[i] = colour;
	}
}

/* Called by PicoDrive just before it lays the 32X layer over visible lines first_line
   onwards (rows screen_offset + first_line onwards). The Mega Drive picture of those rows
   is finished but not yet covered. */
static void on_32x_layer(int first_line, int line_count, int screen_offset, int x32_enabled)
{
	int mode = Pico32x.vdp_regs[0] & P32XV_Mx;
	int buffer = Pico32x.vdp_regs[0x0a / 2] & P32XV_FS;
	const uint16_t *dram = Pico32xMem->dram[buffer];
	static int warned_direct_colour;

	for (int l = 0; l < line_count; l++) {
		int line = first_line + l;
		int row = screen_offset + line;
		if (row < 0 || row >= LAYERS_HEIGHT)
			continue;

		/* The Mega Drive layer: the picture so far, and the CRAM indices PicoDrive keeps
		   for the layer priority (Draw2FB, 328 bytes a row, 8 bytes of margin on the left). */
		memcpy(layers.md_picture[row], &core_screen[row * CORE_SCREEN_WIDTH], sizeof(layers.md_picture[row]));
		const uint8_t *md_indices = Pico.est.Draw2FB + 328 * row + 8;
		for (int x = 0; x < LAYERS_WIDTH; x++)
			layers.md_index[row][x] = md_indices[x] & 0x3F;
		layers.md_background[row] = Pico.video.reg[7] & 0x3F;
		row_captured[row] = 1;

		layers.x32_shown[row] = x32_enabled;
		if (!x32_enabled)
			continue;

		uint16_t line_start = dram[line];
		if (mode == X32_PACKED_PIXEL) {
			decode_packed_line(dram, line_start, layers.x32_index[row]);
			layers.x32_hud_types = hud_drawn_types(buffer);
			for (int x = 0; x < LAYERS_WIDTH; x++) {
				layers.x32_keep[row][x] = hud_keep_framebuffer_pixel(buffer, line, x);
				layers.x32_hud_item[row][x] = hud_item(buffer, line, x);
			}
		} else if (mode == X32_RUN_LENGTH) {
			decode_run_length_line(dram, line_start, layers.x32_index[row]);
			memset(layers.x32_keep[row], 1, LAYERS_WIDTH);
			memset(layers.x32_hud_item[row], 0, LAYERS_WIDTH);
		} else {
			/* ponytail: direct colour (15-bit pixels, no palette) is not used by the game;
			   add a colour plane to layers_t if another screen turns out to use it. */
			if (!warned_direct_colour)
				fprintf(stderr, "layers: 32X direct colour mode is not supported\n");
			warned_direct_colour = 1;
			layers.x32_shown[row] = 0;
			continue;
		}
		copy_32x_palette(layers.x32_palette[row]);
	}
}

void layers_init(void)
{
	vrde_32x_layer_hook = on_32x_layer;
}

/* Plane B's size in cells (VDP register 16) */
static int plane_cells(int size_bits)
{
	return size_bits == 1 ? 64 : size_bits == 3 ? 128 : 32;
}

/* A pixel of a tile in VRAM (PicoDrive keeps it as 16-bit words): 4 bits, the leftmost
   pixel in the high bits */
static int tile_pixel(int tile, int x, int y)
{
	uint16_t word = PicoMem.vram[(tile * 32 + y * 4 + (x / 4) * 2) / 2];
	return (word >> (12 - 4 * (x % 4))) & 15;
}

int layers_plane_b_scroll(void)
{
	int hscroll_table = (Pico.video.reg[13] & 0x3F) << 10;
	return PicoMem.vram[(hscroll_table + 2) / 2] & 0x3FF;
}

/* Plane B into md_wide, as the VDP would show it on a screen SKY_WIDTH wide. Only the
   whole-screen scroll modes the races use; ponytail: per-line or per-cell scroll would
   need the scroll of each line here. */
static void draw_plane_b_wide(void)
{
	const unsigned char *reg = Pico.video.reg;
	int nametable = (reg[4] & 7) << 13;
	int width = plane_cells(reg[16] & 3), height = plane_cells((reg[16] >> 4) & 3);
	int hscroll = layers_plane_b_scroll();
	int vscroll = PicoMem.vsram[1] & 0x3FF;
	int background = reg[7] & 0x3F;
	layers.md_wide_scroll = hscroll;
	layers.md_wide_first_line = -vscroll & (height * 8 - 1);
	layers.md_wide_lines = height * 8;

	for (int row = 0; row < LAYERS_HEIGHT; row++) {
		int line = row - core_visible_top;
		int py = (line + vscroll) & (height * 8 - 1);
		for (int i = 0; i < SKY_WIDTH; i++) {
			int px = (i - SKY_MARGIN - hscroll) & (width * 8 - 1);
			uint16_t entry = PicoMem.vram[(nametable + ((py / 8) * width + px / 8) * 2) / 2];
			int tx = entry & 0x0800 ? 7 - px % 8 : px % 8;   /* flipped across, down */
			int ty = entry & 0x1000 ? 7 - py % 8 : py % 8;
			int pixel = tile_pixel(entry & 0x7FF, tx, ty);
			int index = pixel ? ((entry >> 13) & 3) * 16 + pixel : background;
			layers.md_wide[row][i] = Pico.est.HighPal[index];
		}
	}
}

void layers_frame_start(void)
{
	memset(row_captured, 0, sizeof(row_captured));
	draw_plane_b_wide();
}

void layers_frame_end(void)
{
	for (int row = 0; row < LAYERS_HEIGHT; row++) {
		if (row_captured[row])
			continue;
		memcpy(layers.md_picture[row], &core_screen[row * CORE_SCREEN_WIDTH], sizeof(layers.md_picture[row]));
		layers.x32_shown[row] = 0;
	}
}
