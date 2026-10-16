/* The HUD graphics in the 32X frame buffer. In a race the master SH2 copies them from the
   ROM into the frame buffer after the slave has drawn the 3D (position, time, speed, the
   rev counter's dial; the scene's +800 list). Watching its writes tells which pixels of
   a frame buffer are HUD, so that the front end can lay them over the HLE's 3D. */
#ifndef VRDE_HUD_H
#define VRDE_HUD_H

#include <stdint.h>

#define HUD_FRAMEBUFFER_BYTES 0x20000

/* Call from the core's SH2 access hook (pico.h) */
void hud_note_sh2_access(int is_slave, unsigned int address, unsigned int value, int size, int is_write);

/* 1 if the byte at this offset of 32X frame buffer 0 or 1 is a HUD pixel: the master wrote
   it since the SH2s started drawing into that buffer, and not as a transparent 0 */
int hud_pixel(int buffer, uint32_t byte_offset);

/* 1 if, in a race picture, the pixel at (x, line) must come from the frame buffer itself
   rather than from the HLE's 3D: a HUD pixel, or a line that the 32X shows from somewhere
   else than where the SH2s draw that line's 3D (the game blanks lines this way when a
   race starts and ends, by pointing them at a black line in the line table). */
int hud_keep_framebuffer_pixel(int buffer, int line, int x);

/* Which entry of the scene's HUD list (+800, 32 entries) wrote the HUD pixel at (x, line)
   of a 3D line, plus 1; 0 if it is not a HUD pixel */
int hud_item(int buffer, int line, int x);

/* The types of the HUD list's entries (each entry's first byte) that drew into frame
   buffer 0 or 1: bit n for type n. The race's HUD uses 1, 2, 4, 6 and 9, EXTEND TIME 3;
   the screens after a race (RESULT, GAME OVER) 7, 8 and 10. */
uint32_t hud_drawn_types(int buffer);

#endif
