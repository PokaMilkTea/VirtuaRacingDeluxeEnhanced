/* The HUD graphics in the 32X frame buffer (see hud.h). */
#include <string.h>

#include "pico/pico_int.h"
#include "hud.h"

#define CACHE_THROUGH       0x20000000
#define FRAMEBUFFER_ADDRESS 0x04000000
#define OVERWRITE_AREA      0x04020000   /* the same memory, but 0 bytes are not written */
#define FRAMEBUFFER_END     0x04040000

static uint8_t written[2][HUD_FRAMEBUFFER_BYTES];
static uint8_t written_item[2][HUD_FRAMEBUFFER_BYTES];   /* hud_item's value for each byte */
static uint32_t drawn_types[2];   /* hud_drawn_types */
static int target = -1;   /* the buffer the SH2s were writing into */

/* The master reads each entry of the scene's HUD list (16 bytes each) just before it
   copies that entry's graphic (06003CB4): the entry being drawn */
#define HUD_LIST_ADDRESS 0x0600C800
#define HUD_LIST_END     0x0600CA00
#define HUD_ENTRY_SIZE   16
static int current_item;   /* entry + 1 */
static int current_type;   /* its type, the entry's first byte */
#define SDRAM_START 0x06000000

/* The SH2s write into the frame buffer not on screen; PicoDrive points p_dram at it
   (Pico32xSwapDRAM). */
static int target_buffer(void)
{
	return msh2.p_dram == Pico32xMem->dram[1];
}

void hud_note_sh2_access(int is_slave, unsigned int address, unsigned int value, int size, int is_write)
{
	address &= ~CACHE_THROUGH;
	if (is_slave)
		return;
	if (!is_write && address >= HUD_LIST_ADDRESS && address < HUD_LIST_END) {
		current_item = (address - HUD_LIST_ADDRESS) / HUD_ENTRY_SIZE + 1;
		uint32_t entry = HUD_LIST_ADDRESS + (current_item - 1) * HUD_ENTRY_SIZE - SDRAM_START;
		current_type = ((const uint16_t *)Pico32xMem->sdram)[entry / 2] >> 8;
		return;
	}
	if (!is_write || address < FRAMEBUFFER_ADDRESS || address >= FRAMEBUFFER_END)
		return;

	/* A new picture: what was written into this buffer before belongs to an old one */
	int buffer = target_buffer();
	if (buffer != target) {
		target = buffer;
		memset(written[buffer], 0, sizeof(written[buffer]));
		memset(written_item[buffer], 0, sizeof(written_item[buffer]));
		drawn_types[buffer] = 0;
	}

	int overwrite = address >= OVERWRITE_AREA;
	for (int i = 0; i < size; i++) {
		unsigned int byte = (value >> (8 * (size - 1 - i))) & 0xFF;
		if (overwrite && byte == 0)
			continue;   /* transparent: the pixel keeps what was there */
		written[buffer][(address + i) % HUD_FRAMEBUFFER_BYTES] = byte != 0;
		written_item[buffer][(address + i) % HUD_FRAMEBUFFER_BYTES] = byte != 0 ? current_item : 0;
		if (byte != 0 && current_type < 32)
			drawn_types[buffer] |= 1u << current_type;
	}
}

int hud_pixel(int buffer, uint32_t byte_offset)
{
	return written[buffer][byte_offset % HUD_FRAMEBUFFER_BYTES];
}

/* The slave draws line y's 3D at byte 0x4000 + 512 y of the frame buffer (overlay C,
   C0000138); the line table at the buffer's start says where the 32X takes each line from
   (in words). */
#define FIRST_3D_LINE_WORD 0x2000
#define WORDS_PER_3D_LINE  0x100

int hud_keep_framebuffer_pixel(int buffer, int line, int x)
{
	const uint16_t *dram = Pico32xMem->dram[buffer];
	uint16_t line_start = dram[line];
	if (line_start != FIRST_3D_LINE_WORD + WORDS_PER_3D_LINE * line)
		return 1;
	return hud_pixel(buffer, (uint32_t)line_start * 2 + x);
}

uint32_t hud_drawn_types(int buffer)
{
	return drawn_types[buffer];
}

int hud_item(int buffer, int line, int x)
{
	const uint16_t *dram = Pico32xMem->dram[buffer];
	uint16_t line_start = dram[line];
	if (line_start != FIRST_3D_LINE_WORD + WORDS_PER_3D_LINE * line)
		return 0;
	return written_item[buffer][((uint32_t)line_start * 2 + x) % HUD_FRAMEBUFFER_BYTES];
}
