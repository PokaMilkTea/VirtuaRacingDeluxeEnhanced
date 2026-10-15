/* Comparing the HLE's 3D with the SH2s' (see hlecheck.h). */
#include "pico/pico_int.h"
#include "hlecheck.h"
#include "hud.h"

#define WIDTH  320

/* The frame buffer on screen */
static int shown_buffer(void)
{
	return Pico32x.vdp_regs[0x0a / 2] & P32XV_FS;
}

/* The palette index of a pixel of the frame buffer on screen (packed pixel mode), and
   its byte's offset in the buffer */
static int framebuffer_pixel(int x, int y, uint32_t *byte_offset)
{
	const uint16_t *dram = Pico32xMem->dram[shown_buffer()];
	*byte_offset = ((uint32_t)dram[y] * 2 + x) % HUD_FRAMEBUFFER_BYTES;
	uint16_t pair = dram[*byte_offset / 2];
	return (*byte_offset & 1) ? (pair & 0xFF) : (pair >> 8);
}

int hlecheck_is_compared(const hle_frame_t *frame, const uint8_t *image, int x, int y, int *framebuffer_index)
{
	uint32_t byte_offset;
	int theirs = framebuffer_pixel(x, y, &byte_offset);
	int ours = image[y * WIDTH + x];
	*framebuffer_index = theirs;
	if (y < frame->view_top || y > frame->view_bottom)
		return 0;
	if (ours == 0 && theirs == 0)
		return 0;
	return !hud_keep_framebuffer_pixel(shown_buffer(), y, x);
}

void hlecheck_compare(const hle_frame_t *frame, const uint8_t *image, int *compared, int *same)
{
	*compared = *same = 0;
	for (int y = frame->view_top; y <= frame->view_bottom; y++) {
		for (int x = 0; x < WIDTH; x++) {
			int theirs;
			if (!hlecheck_is_compared(frame, image, x, y, &theirs))
				continue;
			(*compared)++;
			*same += image[y * WIDTH + x] == theirs;
		}
	}
}
