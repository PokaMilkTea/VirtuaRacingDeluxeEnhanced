/* Runs the original game on the PicoDrive emulator core.
   Shared by every program in this project. */
#ifndef VRDE_CORE_H
#define VRDE_CORE_H

#include <stdint.h>

/* The screen buffer the core draws into. Only part of it is visible (see below). */
#define CORE_SCREEN_WIDTH  320
#define CORE_SCREEN_HEIGHT 240

/* The finished picture of the last frame: Mega Drive and 32X layers already combined.
   One uint16_t per pixel in RGB565, CORE_SCREEN_WIDTH pixels per row. */
extern uint16_t core_screen[CORE_SCREEN_WIDTH * CORE_SCREEN_HEIGHT];

/* Which rows of core_screen hold the picture (224 rows on an NTSC console). */
extern int core_visible_top;
extern int core_visible_lines;

/* The sound of the last frame: core_audio_frame_count stereo frames,
   stored as left, right, left, right, ... 16-bit samples. */
#define CORE_AUDIO_MAX_FRAMES 2048
extern int16_t core_audio_samples[2 * CORE_AUDIO_MAX_FRAMES];
extern int core_audio_frame_count;

/* Loads the ROM and powers the console on.
   Returns 0 on success; on failure prints the reason and returns -1. */
int core_init(const char *rom_path, int sample_rate);

/* Runs one frame (1/60 s) with player 1 holding the given buttons.
   Each button is one bit: 1 << GBTN_* from platform/common/input_pico.h. */
void core_run_frame(uint16_t buttons);

/* Buttons for an input script, from letters such as "SB": any of S A B C U D L R X Y Z,
   and M for Mode (S = Start, U/D/L/R = up/down/left/right); "" = none.
   Exits on an unknown letter. */
uint16_t core_parse_buttons(const char *letters);

/* The reverse: the letters for these buttons, in the order S A B C U D L R X Y Z M, into
   letters (at least 13 bytes) */
void core_button_letters(uint16_t buttons, char *letters);

/* The emulator's whole state to a file, and back. Both return 0 on failure. */
int core_save_state(const char *path);
int core_load_state(const char *path);

#endif
