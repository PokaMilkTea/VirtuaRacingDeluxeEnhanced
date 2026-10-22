/* vrde: the Virtua Racing Deluxe front end (SDL2 + OpenGL 3.3 core), on the PicoDrive
   emulator core. Adapted from the MD version's front end (vrr).

   usage: vrde [-v] [-d N [-t F] [-r R] [-f]] [-l state] [-p frame:keys]... [-o file] rom.32x
     -v    print the version and exit
     -d    run frames 0..N as fast as possible in a hidden window (input only from -p),
           checking on every frame that our composite of the two layers (layers.c, drawn
           by the GPU) is exactly the core's frame. Frame N is written as the core drew it
           (vrde_NNNNNN.ppm), as we composited it (composite_NNNNNN.ppm) and, in a race,
           with the HLE's 3D (hle_NNNNNN.ppm). Then exits.
     -t    with -d: from frame F on, also check the HLE. On every race frame, the composite
           with the HLE's 3D filled by the SH2s' own rules (sh2raster.c) must be exactly the
           core's frame; and each time a new race picture comes on screen, the 3D as the
           GPU draws it is compared with the SH2s' pixel by pixel (hlecheck.c), printing
           the share of equal pixels
     -r    with -d: in a race, also write frame N with the HLE's 3D at R times the console's
           resolution with MSAA (hle_NNNNNN_xR.ppm), to look at
     -f    with -d: the HLE's exact vertices instead of the SH2s' (key 4); -t then measures
           how far the GPU's picture with them is from the SH2s'
     -b    with -d -r: that picture as played: the dither blended (key 5), the 32X's own
           colours (picodrive_green 0), smooth motion (key 6, the state at the end of frame N),
           16:9 (key 7), the far view with haze (key 8) and detailed opponents (key 9)
     -l    start from a saved state (key O writes one to vrde.mds); with -d, frames and -p
           count from there
     -p    from that frame on, hold these buttons (S A B C U D L R X Y Z, M for Mode;
           "-p 900:" releases everything), as in vrdetrace
     -o    while playing, write the input to this file as -p arguments, from the first
           frame (or from the state -l loads): vrdetrace given them plays the same game
           again, frame for frame. Loading a state while playing ends the record there

   The picture: the core draws the Mega Drive layer and lays the 32X layer over it; we keep
   both layers (layers.c) and composite them on the GPU by the same rule. In a race the
   3D view of the 32X layer (rows 16-207) is replaced by the HLE's 3D (hle.c), drawn by the
   GPU from the scene the 68000 sends; the HUD graphics the master SH2 copies in stay
   (hud.c). Key 1 cycles the view: our composite, the core's own frame, or both side by
   side (in vrde.ini, hle 0 shows the SH2s' 3D instead of the HLE's); key 2 cycles the
   HLE's internal resolution: 1x, 2x, 4x, or the window's (as many pixels as the picture
   has on screen, the default); key 3 turns MSAA on (the default) and off; key 4 switches
   the vertices between computed exactly in floating point (the default: no jitter at a
   high resolution) and the SH2s' integers (in vrde.ini,
   picodrive_green 1 shows the 32X's colours as PicoDrive does, the palette's priority bit
   in the lowest bit of green); key 5 switches, in a race, between
   blending the dither (the default: shadows and smoke half transparent, the sky's clouds
   as gradients) and the original dither pattern, which stays on the console's pixel grid
   at any resolution; key 6 cycles the race's 3D through the 32X's 20 pictures a second
   and moving smoothly at 30, 40 or 60 images a second or at the display's rate (the default:
   interpolated between the game's 20 ticks a second, with exact vertices whatever key 4
   says); key 7 cycles
   the picture's shape: 16:9 (the default: the race's 3D and sky wider, Hor+, and over the
   black borders; other screens stay 4:3 in the middle), 21:9 (43:18), the window's (up to
   32:9; side by side, the right half's) and the console's 4:3; key 8 cycles the
   far view: on with haze (the default: the track twice as far, hle_set_far_view, fading
   into the sky with distance), on, and off; key 9 switches the opponents between all
   detailed (the default: the model and wheels the game gives only the near ones,
   hle_set_detailed_opponents) and the game's simpler models further away.
   All are kept in vrde.ini (in the current directory, on Windows beside vrde.exe).

   Timing: when the display runs within 0.8% of 59.92 Hz the emulator does one frame per
   vsync, of twice that one frame every second vsync; otherwise it falls back to the
   console's 59.92 Hz with a time accumulator. In vsync mode the emulated frame rate is the
   vsync rate measured at start, and the vsyncs passed are counted in whole periods from a
   moment of a vsync (which slowly follows their phase), not swap by swap, so irregular
   swaps neither add nor lose frames. Audio goes into an SDL
   queue; each frame's samples are stretched or shrunk by up to 1% (linear resampling) to
   keep the queue near its target, so it neither runs dry (crackles) nor grows (latency);
   past three times the target, frames of sound are dropped. Timing statistics are printed
   on exit.

   Keys, as in the MD version: arrows, A/S/D = A/B/C (B is the accelerator in a race),
   F = Start, Z/X/C = X/Y/Z (camera views), V = Mode. A game controller works alongside
   (read_controller). 1 to 0 and - change the view (above). O saves the emulator's state to
   vrde.mds, P loads it. Alt+Enter toggles fullscreen. Esc (Back or Guide on a controller)
   pauses the game and opens a menu with the settings above, the analog controls,
   fullscreen, saving, loading and quitting, as in the MD version; opening it reads vrde.ini
   again, so that it can be edited while the game runs. The window's title shows the name,
   the version and the pictures drawn a second. Both of a controller's sticks pressed show
   the debug info (sizes, frame rates, the analog controls) or hide it again. */
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <SDL.h>

#include "pico/pico_int.h"
#include "platform/common/input_pico.h"
#include "core.h"
#include "gl.h"
#include "hle.h"
#include "hlecheck.h"
#include "hud.h"
#include "sh2raster.h"
#include "layers.h"
#include "settings.h"
#include "analog.h"
#include "version.h"

#define TITLE "Virtua Racing Deluxe Enhanced " VRDE_VERSION   /* the window's, and -v's */

#ifdef _WIN32
#include <windows.h>

/* Laptops with two GPUs (NVIDIA Optimus, AMD switchable graphics) run a program on the
   integrated one unless it asks for the fast one by exporting these */
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

/* On Windows a crash ends the program without a word: say what happened and where, as an
   offset into vrde.exe (llvm-nm or llvm-symbolizer on the same build turn it into a
   function) */
static LONG WINAPI report_crash(EXCEPTION_POINTERS *exception)
{
	char *base = (char *)GetModuleHandle(NULL);
	char *address = exception->ExceptionRecord->ExceptionAddress;
	fprintf(stderr, "vrde crashed: exception %08lx at vrde.exe+%llx\n",
		exception->ExceptionRecord->ExceptionCode, (unsigned long long)(address - base));
	return EXCEPTION_EXECUTE_HANDLER;
}
#endif

#define SAMPLE_RATE   48000
#define NTSC_FPS      (53693175.0 / (3420.0 * 262.0))  /* 59.9228, the console's NTSC rate */
#define AUDIO_TARGET  (SAMPLE_RATE / 15)              /* aim for ~67 ms of queued audio */
#define AUDIO_NUDGE   0.01                            /* at most 1% more or fewer samples */
#define VSYNC_LOCK    0.008                           /* tie frames to vsyncs when that is within
                                                         0.8% of the console's rate (under the nudge) */
#define AUDIO_LIMIT   (3 * AUDIO_TARGET)              /* past this, frames are dropped to catch up */
#define PIXEL_ASPECT  (32.0 / 35.0)    /* a 320-pixel-wide line on a 4:3 TV (the MD's H40 mode) */
#define MAX_SCRIPT    256

/* ---- settings (the menu, keys 1 to 9 and vrde.ini) ---- */

enum { VIEW_COMPOSITE, VIEW_ORIGINAL, VIEW_SIDE_BY_SIDE, VIEW_COUNT };

static int view = VIEW_COMPOSITE;   /* key 1 */
static int hle_on = 1;              /* vrde.ini only: 0 the SH2s' 3D */
static int resolution = 0;          /* key 2: the HLE's 3D at 1, 2 or 4 times the 32X's, or: */
#define RESOLUTION_WINDOW 0         /* as many pixels as the picture has on screen */
static int msaa = 1;                /* key 3: the HLE's 3D anti-aliased (MSAA) */
static int exact_vertices = 1;      /* key 4: the HLE's vertices computed exactly, or as the SH2s
                                       round them (hle_polygon_t's fx, fy or x, y) */
static int picodrive_green = 0;     /* vrde.ini only: the 32X's colours with the priority bit in green,
                                       as PicoDrive shows them (COLOUR_FUNCTIONS) */
static int smooth = 1;              /* key 6: the race's 3D moving smoothly between the game's
                                       ticks (hle_interpolated), with exact vertices: at the
                                       display's rate, or 30, 40 or 60 images a second; or as the
                                       32X shows it, 20 a second */
#define SMOOTH_OFF     0               /* the numbers are those of the MD version */
#define SMOOTH_DISPLAY 1
#define SMOOTH_40      2               /* two images a tick */
#define SMOOTH_60      3               /* three: one an emulated frame */
#define SMOOTH_30      4               /* one every two emulated frames */
static int widescreen = 1;          /* key 7: a wider picture (the race's 3D and sky wider, and
                                       over the black borders above and below), or 4:3: */
#define ASPECT_4_3   0
#define ASPECT_16_9  1
#define ASPECT_21_9  2                 /* 43:18, as 3440x1440 */
#define ASPECT_AUTO  3                 /* the window's shape (picture_aspect) */
static int blend_dither = 1;        /* key 5: in a race, the 3D's dithered polygons (shadows,
                                       smoke) drawn half transparent and the sky's dither
                                       blended (skyblend_shader), or both dithered as the original */
static int detailed_opponents = 1;   /* key 9: hle_set_detailed_opponents */
static int far_view = 1;            /* key 8: in a race, the track twice as far
                                       (hle_set_far_view), with haze or without */
#define FAR_VIEW_OFF  0
#define FAR_VIEW_HAZE 1                /* far away the 3D fades into what lies behind it */
#define FAR_VIEW_ON   2                /* without the haze */
static int far_tiles = 12;          /* vrde.ini only: the far view adds track and props in the
                                       tiles of the course's grid (1024 world units square) so
                                       many steps from the car's along the course
                                       (hle_set_far_view_tiles), and reaches as far
                                       (far_view_reach) */
static int haze_start = 8;          /* vrde.ini only: the haze starts at this depth, in thousands
                                       of world units (the original draws up to about 4), */
static int haze_end = 10;           /* and hides everything from this one on (haze_end_units) */
static int debug_info = 0;          /* sizes, frame rates and the analog controls at the top
                                       left (draw_debug_info): both sticks pressed in turn it on
                                       and off (toggle_debug_info); not kept in vrde.ini */
static int fullscreen = 0;          /* the menu's Fullscreen (Alt+Enter), as the player left it:
                                       the game starts that way (main) */
static int analog = 1;              /* the menu's Analog control: the left stick steers and
                                       the triggers press the throttle and the brake by how
                                       far they go (analog.c), or all three as the buttons */
static int stick_dead_zone = 8;     /* with analog, the stick's dead zone in percent of its way
                                       out; within it the D-pad steers */
static int trigger_dead_zone = 5;   /* and the triggers'; within it the buttons work the pedals */
#define DEAD_ZONE_MAX 30
#define TILE_UNITS 1024             /* a tile of the course's grid, in world units */
#define MSAA_SAMPLES 4              /* fewer if the GL offers fewer */

static const setting settings[] = {
	{ "view", &view, (1u << VIEW_COUNT) - 1, "1: 0 our composite, 1 the core's frame, 2 side by side" },
	{ "hle", &hle_on, 3, "the race's 3D: 1 drawn by the GPU from the scene (HLE), 0 the SH2s' picture" },
	{ "resolution", &resolution, (1u << 0) | (1u << 1) | (1u << 2) | (1u << 4),
		"2: the HLE's 3D at 1, 2 or 4 times the console's resolution, 0 the window's" },
	{ "msaa", &msaa, 3, "3: 1 the HLE's 3D anti-aliased (MSAA), 0 not" },
	{ "exact_vertices", &exact_vertices, 3, "4: the HLE's vertices: 1 computed exactly, 0 rounded as the SH2s do" },
	{ "picodrive_green", &picodrive_green, 3, "the 32X's colours: 0 as the 32X has them, 1 as PicoDrive shows them (priority bit in green)" },
	{ "blend_dither", &blend_dither, 3, "5: in a race, 1 the dithered shadows, smoke and sky blended, 0 dithered as the original" },
	{ "smooth", &smooth, 15, "6: the race's 3D moving smoothly between the game's ticks: 1 at the display's rate, 4 at 30 images a second, 2 at 40, 3 at 60, 0 as the game's 20" },
	{ "widescreen", &widescreen, 15, "7: the picture: 1 16:9, 2 21:9 (43:18), 3 the window's shape (the race wider), 0 the console's 4:3" },
	{ "far_view", &far_view, 7, "8: in a race, 1 the track twice as far with haze, 2 without, 0 as far as the original" },
	{ "detailed_opponents", &detailed_opponents, 3, "9: in a race, 1 every opponent with the near ones' detailed model, 0 the game's simpler ones further away" },
	{ "fullscreen", &fullscreen, 3, "the menu's Fullscreen (Alt+Enter): 1 the game starts fullscreen, 0 in a window" },
	{ "analog", &analog, 3, "the menu's Analog control: 1 the controller's left stick steers and its triggers press the throttle (right) and the brake (left) by how far they go, 0 all three in the buttons' steps as the original" },
	{ "stick_dead_zone", &stick_dead_zone, (1u << (DEAD_ZONE_MAX + 1)) - 1, "with analog, the stick's dead zone in percent of its way out (0-30); within it the D-pad steers" },
	{ "trigger_dead_zone", &trigger_dead_zone, (1u << (DEAD_ZONE_MAX + 1)) - 1, "with analog, the triggers' dead zone in percent (0-30); within it the buttons work the pedals" },
	{ "far_tiles", &far_tiles, ~0u, "the far view adds track and props up to this many tiles (1024 world units) along the course from the car, and reaches as far (4-16; the original sends 3 ahead)" },
	{ "haze_start", &haze_start, ~0u, "the haze (far_view 1) starts at this depth, in thousands of world units (0-31; the original draws up to about 4)" },
	{ "haze_end", &haze_end, ~0u, "... and hides everything from this depth on, in thousands (1-31, at most the far view's end); 0 half a tile before the far view's end" },
};
#define SETTING_COUNT ((int)(sizeof(settings) / sizeof(settings[0])))

/* The vertices in use: exact ones (key 4), always with smooth motion (key 6), where the
   SH2s' rounding would make everything jitter by a pixel as it moves a little each
   frame */
static int using_exact_vertices(void)
{
	return exact_vertices || smooth;
}

/* The in-game menu (see "settings, the in-game menu and the keys"): while it is open the
   game is paused */
static struct {
	int open;
	int selected;              /* the item under the cursor */
	const char *status;        /* shown beside the selected item after saving or loading */
	int wait_for_release;      /* after closing it: no game input until every button is up */
} menu;

#define MENU_COLUMNS 32        /* characters a line */
#define MENU_LINES   18        /* the title, a gap, 9 settings, a gap, 5 items, the hint */
#define CHAR_W       8         /* a character's cell: a 5 x 7 glyph and its outline */
#define CHAR_H       12
#define MENU_W       (MENU_COLUMNS * CHAR_W + 16)   /* the panel, 8 pixels of margin all round */
#define MENU_TOP     4         /* margin above and below: the panel then fits 3 times in the
                                  window as it opens (672 high) */
#define MENU_H       (MENU_LINES * CHAR_H + 2 * MENU_TOP)

/* vrde's own files (vrde.ini, vrde.mds). On Windows they go beside vrde.exe: started from
   Explorer, say with a ROM dropped on it, the program gets a current directory the player
   never chose. Elsewhere they go to the current directory. The path is in a buffer that
   the next call reuses. */
static const char *data_path(const char *name)
{
#ifdef _WIN32
	static char path[1024];
	char *base = SDL_GetBasePath();       /* the folder of vrde.exe, ending in a separator */
	snprintf(path, sizeof(path), "%s%s", base ? base : "", name);
	SDL_free(base);
	return path;
#else
	return name;
#endif
}

#define SETTINGS_FILE data_path("vrde.ini")
#define STATE_FILE    data_path("vrde.mds")

static void save_settings(void)
{
	settings_save(SETTINGS_FILE, "vrde settings: the menu (Esc) and keys 1 to 9 change most of these while playing, and this file is rewritten",
		settings, SETTING_COUNT);
}

/* Writes the file when there is none */
static void load_settings(void)
{
	if (!settings_load(SETTINGS_FILE, settings, SETTING_COUNT))
		save_settings();
}

/* How far the far view reaches, in world units: far_tiles tiles, within the HLE's
   limits (hle_set_far_view_distance) */
static int far_view_reach(void)
{
	int reach = far_tiles * TILE_UNITS;
	return reach < FAR_VIEW_NEAREST ? FAR_VIEW_NEAREST : reach > FAR_VIEW_FURTHEST ? FAR_VIEW_FURTHEST : reach;
}

/* Where the haze hides everything, in world units: haze_end, or by default half a tile
   before the far view's end, since the window's front edge (tiles from the car; the end is
   a depth from the camera) leaves tiles out from about 7850 for 8 tiles */
static float haze_end_units(void)
{
	float reach = (float)far_view_reach();
	if (haze_end == 0)
		return reach - TILE_UNITS / 2;
	return fminf(haze_end * 1000.0f, reach);
}

/* The settings the HLE keeps, from ours: at start, and each time the menu opens */
static void apply_hle_settings(void)
{
	hle_set_far_view(far_view != FAR_VIEW_OFF);
	hle_set_far_view_distance(far_view_reach());
	hle_set_far_view_tiles(far_tiles);
	hle_set_detailed_opponents(detailed_opponents);
}

/* ---- saved states ---- */

static double now_seconds(void);

/* A line shown at the window's top left for a moment (draw_notice) */
#define NOTICE_SECONDS 1.0
static struct {
	const char *text;
	double until;
} notice;

static void show_notice(const char *text)
{
	notice.text = text;
	notice.until = now_seconds() + NOTICE_SECONDS;
}

/* The analog controls kept for the replay, beside the state (analog.h) */
static const char *analog_record_path(const char *state_path)
{
	static char path[1100];
	snprintf(path, sizeof(path), "%s.analog", state_path);
	return path;
}

static int save_state(void)
{
	int saved = core_save_state(STATE_FILE) && analog_save_record(analog_record_path(STATE_FILE));
	if (saved)
		printf("state saved to %s\n", STATE_FILE);
	show_notice(saved ? "State saved" : "State not saved");
	return saved;
}

/* -o: the input as vrdetrace's arguments, one line each time it changes: -p the buttons,
   -j the stick's steering target, -g and -b the throttle's and the brake's triggers */
static FILE *input_record;
static long input_record_frame;            /* the frame about to run, counted as -p counts */
static int input_recorded_buttons = -1;   /* -1 = nothing written yet */
static int input_recorded_analog[3];      /* steering, throttle, brake */

static void record_value(int *recorded, int value, char option)
{
	if (value != *recorded) {
		fprintf(input_record, "-%c %ld:%d\n", option, input_record_frame, value);
		*recorded = value;
	}
}

/* Called once per emulated frame, with the buttons and the analog controls it runs with */
static void record_input(uint16_t buttons, int steering, int throttle, int brake)
{
	if (input_record) {
		if (buttons != input_recorded_buttons) {
			char letters[16];
			core_button_letters(buttons, letters);
			fprintf(input_record, "-p %ld:%s\n", input_record_frame, letters);
			input_recorded_buttons = buttons;
		}
		record_value(&input_recorded_analog[0], steering, 'j');
		record_value(&input_recorded_analog[1], throttle, 'g');
		record_value(&input_recorded_analog[2], brake, 'b');
		fflush(input_record);
	}
	input_record_frame++;
}

static void end_input_record(const char *reason)
{
	if (!input_record)
		return;
	fprintf(input_record, "# %s at frame %ld: the record ends here\n", reason, input_record_frame);
	fclose(input_record);
	input_record = NULL;
}

static int load_state(const char *path)
{
	int loaded = core_load_state(path);
	if (loaded)
		end_input_record("a state was loaded");
	if (loaded) {
		analog_load_record(analog_record_path(path));
		hle_reset();
		printf("state loaded from %s\n", path);
	}
	show_notice(loaded ? "State loaded" : "No saved state");
	return loaded;
}

/* ---- OpenGL objects ---- */

/* Texture units, each holding one texture for the whole run */
enum {
	UNIT_CORE_FRAME,     /* the core's own frame (RGB565) */
	UNIT_MD_PICTURE,     /* the Mega Drive layer (RGB565) */
	UNIT_MD_FLAGS,       /* per pixel, see MD_FLAG_* */
	UNIT_X32_INDICES,    /* the 32X layer: palette index per pixel */
	UNIT_X32_PALETTE,    /* the 32X palette of each row: BGR555 + priority, 256 x rows */
	UNIT_HLE_INDICES,    /* the HLE's 3D at native resolution: palette index per pixel, 0 =
	                        nothing (hle_draw_indices; for the -t checks) */
	UNIT_HLE_COLOUR,     /* the HLE's 3D as shown: linear-light colour premultiplied by the
	                        pixel's coverage, at the internal resolution (hle_draw_colour) */
	UNIT_HLE_HAZE,       /* with it, the haze of the nearest 3D (key 8), premultiplied too */
	UNIT_MD_WIDE,        /* in a race, plane B drawn wider than the screen (layers.c), RGB565 */
	UNIT_MD_BLENDED,     /* the same with its dither blended, linear (skyblend_shader) */
	UNIT_MENU,           /* the in-game menu's panel (RGBA, premultiplied) */
};

/* How the Mega Drive layer meets the 32X layer at a pixel */
enum {
	MD_FLAG_PICTURE,     /* the MD pixel shows, unless the 32X colour has priority */
	MD_FLAG_BACKGROUND,  /* the MD pixel is the background colour: the 32X pixel shows */
	MD_FLAG_MD_ONLY,     /* no 32X layer on this row */
};
/* Added to the above: with the HLE on, the 32X pixel stays the frame buffer's (HUD) */
#define MD_FLAG_KEEP_32X 4
/* Added on every pixel of a line the frame buffer keeps whole (the blinds as a race
   starts or ends): widescreen sides show that line too */
#define MD_FLAG_LINE_KEPT 8
/* Bits 4-5: the pixel's HUD group when the HUD moves (HUD_LEFT, ...) */
#define MD_FLAG_HUD_SHIFT 4

static GLuint menu_program;           /* the menu's panel over the picture */
static GLuint blit_program, composite_program, hle_index_program, hle_colour_program, colourize_program;
static GLuint skyblend_program, sky_framebuffer;   /* the sky's dither blended (blend_sky) */
static GLuint quad_vao;            /* no attributes: the vertex shader makes the quad */
static GLuint hle_vao, hle_vertex_buffer;
static GLuint hle_index_framebuffer, hle_colour_framebuffer, hle_colour_texture, hle_haze_texture;
static int hle_colour_w, hle_colour_h;               /* the colour texture's size */
static GLuint msaa_framebuffer, msaa_renderbuffer, msaa_haze_renderbuffer;   /* key 3: the 3D drawn
                                                       here, then resolved */
static int msaa_samples, msaa_w, msaa_h;            /* into the colour texture */

/* The HLE's polygon list of the frame buffer in the layers of the last emulated frame
   (NULL when that is not a race picture) */
static const hle_frame_t *hle_visible;

/* The HLE's image: the 32X frame buffer's size, at native resolution */
#define HLE_IMAGE_WIDTH  320

/* The widest picture: 32:9. Wider windows get black bars. */
#define WIDEST_ASPECT (32.0 / 9.0)
#define WIDEST_SIDE   276              /* its sides, as picture_side gives them */

/* The shape of the area our picture is drawn in (draw): what ASPECT_AUTO follows */
static double area_aspect = 16.0 / 9.0;

/* The picture's shape on screen (width / height) */
static double picture_aspect(void)
{
	switch (widescreen) {
	case ASPECT_16_9: return 16.0 / 9.0;
	case ASPECT_21_9: return 43.0 / 18.0;
	case ASPECT_AUTO: return area_aspect < WIDEST_ASPECT ? area_aspect : WIDEST_ASPECT;
	default:          return 4.0 / 3.0;
	}
}

/* Columns added on each side of the console's picture (Hor+: the same scale, more to
   see), and the picture's width. At the console's pixel aspect 224 lines are
   224 * aspect / (32/35) pixels wide: for 16:9, 436, so 58 columns more on each side of
   the 320; 133 for 43:18. */
static int picture_side(void)
{
	if (widescreen == ASPECT_4_3)
		return 0;
	int side = (int)lround((224 * picture_aspect() / PIXEL_ASPECT - HLE_IMAGE_WIDTH) / 2);
	return side < 0 ? 0 : side;
}

static int picture_width(void)
{
	return HLE_IMAGE_WIDTH + 2 * picture_side();
}

/* The visible part of the frame, as the core reports it */
static int frame_width(void) { return CORE_SCREEN_WIDTH; }
static int frame_height(void) { return core_visible_lines; }

/* ---- the HUD in widescreen ----

   In a race the HUD's left group (position, rev counter, speed) moves left by the sides'
   width, its right group (lap time, course map, course name) right, so that they keep to
   the picture's edges; what spans the middle (TIME, the countdown, RESULT) stays. The
   master copies each HUD graphic from the scene's list (hud_item), so each graphic moves
   as one, by where its pixels lie this frame. */
enum { HUD_NONE, HUD_LEFT, HUD_MIDDLE, HUD_RIGHT };
#define HUD_ITEMS       32
#define HUD_LEFT_BELOW  140   /* a graphic wholly left of this column is in the left group */
#define HUD_RIGHT_ABOVE 180   /* wholly right of this one, the right group */

static uint8_t hud_group[HUD_ITEMS + 1];   /* by hud_item's value */

static int group_of_span(int first_x, int last_x)
{
	if (last_x < HUD_LEFT_BELOW)
		return HUD_LEFT;
	if (first_x > HUD_RIGHT_ABOVE)
		return HUD_RIGHT;
	return HUD_MIDDLE;
}

static void group_hud_items(void)
{
	int first_x[HUD_ITEMS + 1], last_x[HUD_ITEMS + 1];
	for (int k = 0; k <= HUD_ITEMS; k++) {
		first_x[k] = LAYERS_WIDTH;
		last_x[k] = -1;
	}
	for (int y = 0; y < frame_height(); y++) {
		int row = core_visible_top + y;
		if (!layers.x32_shown[row])
			continue;
		for (int x = 0; x < LAYERS_WIDTH; x++) {
			int k = layers.x32_hud_item[row][x];
			if (k == 0)
				continue;
			if (x < first_x[k])
				first_x[k] = x;
			if (x > last_x[k])
				last_x[k] = x;
		}
	}
	for (int k = 0; k <= HUD_ITEMS; k++)
		hud_group[k] = last_x[k] < 0 ? HUD_NONE : group_of_span(first_x[k], last_x[k]);
}

/* Whether this frame's HUD moves to the edges: a race picture in widescreen, except the
   screens after a race (RESULT, GAME OVER: HUD entry types 7, 8 and 10), which keep their
   4:3 layout, as RESULT and the place are separate entries. The race's other words
   (EXTEND TIME: type 3) move with the HUD as before. */
static int moving_hud;
#define AFTER_RACE_TYPES (1u << 7 | 1u << 8 | 1u << 10)

/* How far a HUD group moves, in console pixels */
static int hud_shift(int group)
{
	return group == HUD_LEFT ? -picture_side() : group == HUD_RIGHT ? picture_side() : 0;
}

#define HLE_IMAGE_HEIGHT 224

typedef struct {
	float x, y;          /* screen pixels from the top left */
	GLuint index;        /* palette index */
	GLuint dithered;
	float depth;         /* world units in front of the camera, 0 for the HUD (the haze) */
} hle_vertex_t;

/* ---- shaders ---- */

/* A quad over the whole viewport from 4 vertices without attributes (a triangle strip);
   uv runs from 0 to uv_scale, with uv.y = 0 on the top line */
static const char *quad_vertex_shader =
	"#version 330 core\n"
	"uniform vec2 uv_scale;\n"
	"out vec2 uv;\n"
	"void main() {\n"
	"  vec2 corner = vec2(gl_VertexID & 1, gl_VertexID >> 1);\n"   /* (0,0) (1,0) (0,1) (1,1) */
	"  uv = vec2(corner.x, 1.0 - corner.y) * uv_scale;\n"
	"  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

/* The core's frame as it is */
static const char *blit_shader =
	"#version 330 core\n"
	"uniform sampler2D tex;\n"
	"in vec2 uv;\n"
	"out vec4 color;\n"
	"void main() { color = texture(tex, uv); }\n";

/* Shared by the shaders below. The 32X colour is 5 bits each. With picodrive_green it
   comes out as PicoDrive writes it to RGB565, with the priority bit (not a colour) as the
   lowest bit of green: needed to match the core bit for bit. Linear light (for the HLE's
   anti-aliased edges, averaged as light is) by the sRGB curve. */
#define COLOUR_FUNCTIONS \
	"uniform bool picodrive_green;\n" \
	"vec3 x32_colour(uint bgr555) {\n" \
	"  uint red = bgr555 & 31u;\n" \
	"  uint green = (bgr555 >> 5) & 31u;\n" \
	"  uint blue = (bgr555 >> 10) & 31u;\n" \
	"  uint priority = bgr555 >> 15;\n" \
	"  if (!picodrive_green)\n" \
	"    return vec3(float(red), float(green), float(blue)) / 31.0;\n" \
	"  return vec3(float(red) / 31.0, float(green * 2u + priority) / 63.0, float(blue) / 31.0);\n" \
	"}\n" \
	"vec3 to_linear(vec3 c) {\n" \
	"  return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));\n" \
	"}\n" \
	"vec3 to_srgb(vec3 c) {\n" \
	"  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));\n" \
	"}\n"

/* Our composite of the two layers, by PicoDrive's rule (pico/32x/draw.c, do_line_pp):
   the 32X pixel shows where the MD pixel is the background colour or where the 32X colour
   has the priority bit; otherwise the MD pixel shows. With the HLE, its 3D lies over what
   the 32X layer would show without the 3D (index 0, transparent), in linear light; its
   coverage (alpha) is below 1 only on anti-aliased edges. */
#define STRINGIFY(x) #x
#define AS_TEXT(x) STRINGIFY(x)   /* not TEXT: Windows headers have one */
#define SKY_MARGIN_TEXT AS_TEXT(SKY_MARGIN) ".0"
#define EDGE_REPEAT 32   /* the sides that repeat the picture's edge repeat this many columns */

static const char *composite_shader =
	"#version 330 core\n"
	"uniform sampler2D md_picture;\n"
	"uniform sampler2D md_wide;\n"
	"uniform sampler2D md_blended;\n"
	"uniform bool use_sky;\n"                     /* the MD layer from md_wide, moved: */
	"uniform float sky_offset;\n"                 /* this many pixels to the left */
	"uniform bool use_md_blended;\n"
	"uniform usampler2D md_flags;\n"
	"uniform usampler2D x32_indices;\n"
	"uniform usampler2D x32_palette;\n"
	"uniform sampler2D hle_colour;\n"
	"uniform sampler2D hle_haze;\n"
	"uniform bool use_haze;\n"
	"uniform vec2 hle_size;\n"
	"uniform int use_hle;\n"
	"uniform ivec2 hle_lines;\n"
	"uniform vec2 frame_size;\n"                 /* the picture, sides included */
	"uniform int side;\n"                        /* columns added on each side (widescreen) */
	"uniform bool fill_borders;\n"               /* 3D and sky over the black borders too */
	"uniform ivec2 sky_lines;\n"                 /* plane B's first and last line on screen */
	"uniform bool wrap_sides;\n"                 /* no race: the sides repeat the picture's edge */
	"uniform bool move_hud;\n"                   /* the HUD's groups to the picture's edges */
	"in vec2 uv;\n"
	"out vec4 color;\n"
	COLOUR_FUNCTIONS
	"\n"
	/* plane B's row for a line. Over the top border plane B's own top rows (above the
	   3D view, the top of the sky) are stretched to start at line 0, so that the view's
	   lines and the horizon stay where they are; above them plane B shows its bottom rows
	   again (black). Below, plane B goes on (the ground or the sea); past its last line,
	   mirrored. */
	"int sky_row(int line) {\n"
	"  if (!fill_borders) return line;\n"
	"  if (line < hle_lines.x) {\n"
	"    int first = min(sky_lines.x, hle_lines.x);\n"
	"    return first + line * (hle_lines.x - first) / hle_lines.x;\n"
	"  }\n"
	"  if (line > sky_lines.y) return 2 * sky_lines.y + 1 - line;\n"
	"  return line;\n"
	"}\n"
	"\n"
	/* a column of the sides that repeat the picture's edge: the outermost
	   EDGE_REPEAT columns over and over, so that patterns of 8 or 16 pixels (the
	   title's checks) go on unbroken and what lies further in (a logo) does not show twice */
	"int repeat_edge(int x) {\n"
	"  if (x < 0) return " AS_TEXT(EDGE_REPEAT) " - 1 - (-x - 1) % " AS_TEXT(EDGE_REPEAT) ";\n"
	"  if (x >= " AS_TEXT(HLE_IMAGE_WIDTH) ") return " AS_TEXT(HLE_IMAGE_WIDTH) " - " AS_TEXT(EDGE_REPEAT) " + (x - " AS_TEXT(HLE_IMAGE_WIDTH) ") % " AS_TEXT(EDGE_REPEAT) ";\n"
	"  return x;\n"
	"}\n"
	"\n"
	/* the HUD group of a pixel (MD_FLAG_HUD_SHIFT: 1 left, 3 right), and the 32X colour */
	"uint hud_group(ivec2 p) { return (texelFetch(md_flags, p, 0).r >> 4) & 3u; }\n"
	"vec3 x32_pixel(ivec2 p) {\n"
	"  uint index = texelFetch(x32_indices, p, 0).r;\n"
	"  return x32_colour(texelFetch(x32_palette, ivec2(int(index), p.y), 0).r);\n"
	"}\n"
	"\n"
	"vec3 with_hle(vec3 c) {\n"
	"  vec4 hle = texelFetch(hle_colour, ivec2(uv * hle_size), 0);\n"
	/* the haze fades the 3D into what is behind it, as if it covered less */
	"  if (use_haze && hle.a > 0.0) hle *= 1.0 - texelFetch(hle_haze, ivec2(uv * hle_size), 0).r / hle.a;\n"
	"  return hle.a > 0.0 ? to_srgb(to_linear(c) * (1.0 - hle.a) + hle.rgb) : c;\n"
	"}\n"
	"\n"
	"void main() {\n"
	"  vec2 position = uv * frame_size - vec2(float(side), 0.0);\n"   /* in console pixels */
	"  ivec2 pixel = ivec2(floor(position));\n"
	/* the HUD's left group shows `side` columns further left, its right group further right */
	"  if (move_hud) {\n"
	"    ivec2 from_left = ivec2(pixel.x + side, pixel.y), from_right = ivec2(pixel.x - side, pixel.y);\n"
	"    if (from_left.x < " AS_TEXT(HLE_IMAGE_WIDTH) " && hud_group(from_left) == 1u) {\n"
	"      color = vec4(x32_pixel(from_left), 1.0);\n"
	"      return;\n"
	"    }\n"
	"    if (from_right.x >= 0 && hud_group(from_right) == 3u) {\n"
	"      color = vec4(x32_pixel(from_right), 1.0);\n"
	"      return;\n"
	"    }\n"
	"  }\n"
	/* the sides repeat the picture's edge: a picture wholly of the 32X layer with a
	   pattern at the edges (wrap_sides), or, over the 3D, a line the frame buffer keeps
	   whole (MD_FLAG_LINE_KEPT: the blinds) or a line outside the 3D view not filled (the
	   borders while a race starts, the title closing over the demo). Without the 3D every
	   32X line is kept whole, so only wrap_sides counts there. */
	"  bool line_kept = (texelFetch(md_flags, ivec2(0, pixel.y), 0).r & 8u) != 0u;\n"
	"  bool outside_3d = pixel.y < hle_lines.x || pixel.y > hle_lines.y;\n"
	"  if (wrap_sides || (use_hle != 0 && (line_kept || (outside_3d && !fill_borders))))\n"
	"    pixel.x = repeat_edge(pixel.x);\n"
	"  bool inside = pixel.x >= 0 && pixel.x < " AS_TEXT(HLE_IMAGE_WIDTH) ";\n"
	"  vec3 md = inside ? texelFetch(md_picture, pixel, 0).rgb : vec3(0.0);\n"
	"  if (use_sky) {\n"
	"    ivec2 sky = ivec2(int(floor(position.x + " SKY_MARGIN_TEXT " + sky_offset)), sky_row(pixel.y));\n"
	"    md = use_md_blended ? to_srgb(texelFetch(md_blended, sky, 0).rgb) : texelFetch(md_wide, sky, 0).rgb;\n"
	"  }\n"
	"  bool border = fill_borders && (pixel.y < hle_lines.x || pixel.y > hle_lines.y);\n"
	/* a line the 32X layer is off on (leaving a race, the game turns it off) shows only the
	   Mega Drive layer, the sides as well */
	"  bool line_without_32x = (texelFetch(md_flags, ivec2(0, pixel.y), 0).r & 3u) == 2u;\n"
	/* without our 3D (hle 0: the SH2s', 4:3 only) the sides stay black, not the sky alone */
	"  if (!inside && use_hle == 0) {\n"
	"    color = vec4(0.0, 0.0, 0.0, 1.0);\n"
	"    return;\n"
	"  }\n"
	"  if (!inside || border) {\n"                    /* no 32X layer: the sky and the 3D */
	"    color = vec4(use_hle != 0 && !line_without_32x ? with_hle(md) : md, 1.0);\n"
	"    return;\n"
	"  }\n"
	"  uint flags = texelFetch(md_flags, pixel, 0).r;\n"
	"  uint flag = flags & 3u;\n"
	"  bool keep_32x = (flags & 4u) != 0u;\n"         /* MD_FLAG_KEEP_32X */
	"  uint group = (flags >> 4) & 3u;\n"
	"  if (move_hud && (group == 1u || group == 3u))\n"   /* moved away: the 3D shows */
	"    keep_32x = false;\n"
	"  if (flag == 2u) {\n"                                  /* MD_FLAG_MD_ONLY */
	"    color = vec4(md, 1.0);\n"
	"    return;\n"
	"  }\n"
	"  uint index = texelFetch(x32_indices, pixel, 0).r;\n"
	"  bool hle_here = use_hle != 0 && pixel.y >= hle_lines.x && pixel.y <= hle_lines.y && !keep_32x;\n"
	"  if (hle_here)\n"
	"    index = 0u;\n"
	"  uint bgr555 = texelFetch(x32_palette, ivec2(int(index), pixel.y), 0).r;\n"
	"  bool x32_in_front = flag == 1u || (bgr555 & 0x8000u) != 0u;\n"
	"  vec3 c = x32_in_front ? x32_colour(bgr555) : md;\n"
	"  color = vec4(hle_here ? with_hle(c) : c, 1.0);\n"
	"}\n";

/* The HLE's polygons, one triangle at a time in the list's order (painter's algorithm, no
   depth buffer), into an image `scale` times the 32X's 320x224. Positions are in the 32X's
   pixels from the top left; image row 0 is the screen's top line. Dithered polygons draw
   only the 32X pixels where x + y is even, as the SH2s do: the checkerboard stays on the
   console's pixel grid at any resolution. */
static const char *hle_vertex_shader =
	"#version 330 core\n"
	"layout(location = 0) in vec2 position;\n"
	"layout(location = 1) in uint index;\n"
	"layout(location = 2) in uint dithered;\n"
	"layout(location = 3) in float depth;\n"
	"uniform vec2 scale;\n"
	"uniform vec2 image_size;\n"
	"uniform float side;\n"                     /* console columns added on the left */
	"flat out uint palette_index;\n"
	"flat out uint dither;\n"
	"out float inverse_depth;\n"               /* 1 / depth goes linearly across the screen */
	"void main() {\n"
	"  palette_index = index;\n"
	"  dither = dithered;\n"
	"  inverse_depth = depth > 0.0 ? 1.0 / depth : 0.0;\n"
	"  gl_Position = vec4((position + vec2(side, 0.0)) * scale / image_size * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

/* The console pixel a fragment lies in, and whether a dithered polygon leaves it out */
#define DITHER_FUNCTIONS \
	"uniform vec2 scale;\n" \
	"uniform float side;\n" \
	"ivec2 console_pixel() { return ivec2(floor(gl_FragCoord.xy / scale - vec2(side, 0.0))); }\n" \
	"bool dithered_out(uint dither) {\n" \
	"  ivec2 p = console_pixel();\n" \
	"  return dither != 0u && ((p.x + p.y) & 1) != 0;\n" \
	"}\n"

/* Palette indices, at the console's resolution: for the -t checks */
static const char *hle_index_shader =
	"#version 330 core\n"
	"flat in uint palette_index;\n"
	"flat in uint dither;\n"
	"out uint index;\n"
	DITHER_FUNCTIONS
	"void main() {\n"
	"  if (dithered_out(dither))\n"
	"    discard;\n"
	"  index = palette_index;\n"
	"}\n";

/* Colours, as shown: the 32X palette of the console line, in linear light, coverage 1. With
   blend_dither a dithered polygon is drawn whole at coverage 1/2 instead: the SH2s'
   checkerboard keeps half of what lies behind, so it stands for 50% transparency. */
static const char *hle_colour_shader =
	"#version 330 core\n"
	"uniform usampler2D x32_palette;\n"
	"uniform bool blend_dither;\n"
	"uniform bool haze;\n"
	"uniform float haze_start, haze_end;\n"
	"flat in uint palette_index;\n"
	"flat in uint dither;\n"
	"in float inverse_depth;\n"
	"layout(location = 0) out vec4 colour;\n"
	"layout(location = 1) out vec4 haze_out;\n"   /* the haze, premultiplied by coverage, in red */
	COLOUR_FUNCTIONS
	DITHER_FUNCTIONS
	"void main() {\n"
	"  if (!blend_dither && dithered_out(dither))\n"
	"    discard;\n"
	"  uint bgr555 = texelFetch(x32_palette, ivec2(int(palette_index), console_pixel().y), 0).r;\n"
	"  float coverage = blend_dither && dither != 0u ? 0.5 : 1.0;\n"
	"  colour = vec4(to_linear(x32_colour(bgr555)) * coverage, coverage);\n"
	/* the haze grows linearly from haze_start to haze_end (the far view's end); drawn over like the colour, the
	   nearest polygon's counts, so a far hill does not show one further away through it */
	"  float amount = 0.0;\n"
	"  if (haze && inverse_depth > 0.0)\n"
	"    amount = clamp((1.0 / inverse_depth - haze_start) / (haze_end - haze_start), 0.0, 1.0);\n"
	"  haze_out = vec4(amount * coverage, 0.0, 0.0, coverage);\n"
	"}\n";

/* An image of palette indices (UNIT_HLE_INDICES) into colours as hle_colour_shader makes
   them, at the console's resolution: for the -t check of the composite */
static const char *colourize_shader =
	"#version 330 core\n"
	"uniform usampler2D hle_indices;\n"
	"uniform usampler2D x32_palette;\n"
	"out vec4 colour;\n"
	COLOUR_FUNCTIONS
	"void main() {\n"
	"  ivec2 pixel = ivec2(gl_FragCoord.xy);\n"
	"  uint index = texelFetch(hle_indices, pixel, 0).r;\n"
	"  uint bgr555 = texelFetch(x32_palette, ivec2(int(index), pixel.y), 0).r;\n"
	"  colour = index == 0u ? vec4(0.0) : vec4(to_linear(x32_colour(bgr555)), 1.0);\n"
	"}\n";

/* The sky's A-B-A dither blended into gradients, in linear light: plane B as we draw it
   (UNIT_MD_WIDE) into UNIT_MD_BLENDED, once per emulated frame. A port of the MD version's
   background_shader. A dither pixel has the same colour on both sides, one 9-bit step from
   its own, across or down; two different colours either side (a 1-pixel highlight between
   a face and the mortar) is not a dither. Each dither pixel and the pixels around it within
   one step share their colours with tent weights (1 2 1): a checkerboard or striped dither
   comes out half and half, an irregular one keeps the amount of each colour. */
static const char *skyblend_shader =
	"#version 330 core\n"
	"uniform sampler2D md_wide;\n"
	"uniform ivec2 size;\n"                       /* the rows drawn: outside, the nearest edge pixel */
	"out vec4 colour;\n"
	COLOUR_FUNCTIONS
	"ivec3 at(ivec2 p) {\n"                       /* red, green, blue as 0-31, 0-63, 0-31 */
	"  vec3 c = texelFetch(md_wide, clamp(p, ivec2(0), size - 1), 0).rgb;\n"
	"  return ivec3(round(c * vec3(31.0, 63.0, 31.0)));\n"
	"}\n"
	/* a step is 4 of 31 (8 of 63 in green), and up to 0.2 of the whole range counts */
	"bool one_step_apart(ivec3 a, ivec3 b) {\n"
	"  ivec3 d = abs(a - b);\n"
	"  return d.r <= 6 && d.g <= 12 && d.b <= 6;\n"
	"}\n"
	"bool is_dither(ivec2 p) {\n"
	"  p = clamp(p, ivec2(0), size - 1);\n"
	"  ivec3 centre = at(p);\n"
	"  ivec3 left = at(p - ivec2(1, 0)), right = at(p + ivec2(1, 0));\n"
	"  ivec3 above = at(p - ivec2(0, 1)), below = at(p + ivec2(0, 1));\n"
	"  bool across = left == right && left != centre && one_step_apart(left, centre);\n"
	"  bool down = above == below && above != centre && one_step_apart(above, centre);\n"
	"  return across || down;\n"
	"}\n"
	"void main() {\n"
	"  ivec2 p = ivec2(gl_FragCoord.xy);\n"         /* row 0 is the picture's top line */
	"  ivec3 centre = at(p);\n"
	"  bool centre_dither = is_dither(p);\n"
	"  vec3 sum = vec3(0.0);\n"
	"  float total_weight = 0.0;\n"
	"  for (int dy = -1; dy <= 1; dy++)\n"
	"    for (int dx = -1; dx <= 1; dx++) {\n"
	"      ivec2 n = p + ivec2(dx, dy);\n"
	"      ivec3 neighbour = at(n);\n"
	"      if ((dx != 0 || dy != 0) && (!one_step_apart(neighbour, centre) || (!centre_dither && !is_dither(n))))\n"
	"        continue;\n"
	"      float weight = float((2 - abs(dx)) * (2 - abs(dy)));\n"
	"      sum += weight * to_linear(vec3(neighbour) / vec3(31.0, 63.0, 31.0));\n"
	"      total_weight += weight;\n"
	"    }\n"
	"  colour = vec4(sum / total_weight, 1.0);\n"
	"}\n";

/* ---- OpenGL setup ---- */

static GLuint compile_shader(GLenum type, const char *source)
{
	GLint ok;
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		fprintf(stderr, "shader: %s\n", log);
		exit(1);
	}
	return shader;
}

static GLuint link_program(const char *vertex_source, const char *fragment_source)
{
	GLuint program = glCreateProgram();
	glAttachShader(program, compile_shader(GL_VERTEX_SHADER, vertex_source));
	glAttachShader(program, compile_shader(GL_FRAGMENT_SHADER, fragment_source));
	glLinkProgram(program);
	return program;
}

/* Uniform setters for the program in use */
static void set_float(GLuint program, const char *name, float value)
{
	glUniform1f(glGetUniformLocation(program, name), value);
}

static void set_int(GLuint program, const char *name, int value)
{
	glUniform1i(glGetUniformLocation(program, name), value);
}

static void set_vec2(GLuint program, const char *name, float x, float y)
{
	glUniform2f(glGetUniformLocation(program, name), x, y);
}

static void use_texture_unit(int unit)
{
	glActiveTexture(GL_TEXTURE0 + unit);
}

/* A texture on the active unit, with nearest filtering (integer textures need it too) */
static GLuint make_texture(GLint internal_format, int w, int h, GLenum format, GLenum type)
{
	GLuint texture;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexImage2D(GL_TEXTURE_2D, 0, internal_format, w, h, 0, format, type, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	return texture;
}

/* The HLE colour program's outputs: the colour, and the haze */
static const GLenum HLE_OUTPUTS[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };

static void gl_init(void)
{
	blit_program = link_program(quad_vertex_shader, blit_shader);
	composite_program = link_program(quad_vertex_shader, composite_shader);

	glUseProgram(composite_program);
	set_int(composite_program, "md_picture", UNIT_MD_PICTURE);
	set_int(composite_program, "md_wide", UNIT_MD_WIDE);
	set_int(composite_program, "md_blended", UNIT_MD_BLENDED);
	set_int(composite_program, "md_flags", UNIT_MD_FLAGS);
	set_int(composite_program, "x32_indices", UNIT_X32_INDICES);
	set_int(composite_program, "x32_palette", UNIT_X32_PALETTE);
	set_int(composite_program, "hle_colour", UNIT_HLE_COLOUR);
	set_int(composite_program, "hle_haze", UNIT_HLE_HAZE);
	set_vec2(composite_program, "uv_scale", 1, 1);

	glUseProgram(blit_program);
	set_int(blit_program, "tex", UNIT_CORE_FRAME);
	set_vec2(blit_program, "uv_scale", 1, 1);

	menu_program = link_program(quad_vertex_shader, blit_shader);
	glUseProgram(menu_program);
	set_int(menu_program, "tex", UNIT_MENU);
	set_vec2(menu_program, "uv_scale", 1, 1);
	use_texture_unit(UNIT_MENU);
	make_texture(GL_RGBA8, MENU_W, MENU_H, GL_RGBA, GL_UNSIGNED_BYTE);
	glUseProgram(blit_program);

	/* The textures hold the visible rows from their top line; they are as big as the
	   largest picture and drawn from their top-left part. */
	use_texture_unit(UNIT_MD_PICTURE);
	make_texture(GL_RGB8, LAYERS_WIDTH, LAYERS_HEIGHT, GL_RGB, GL_UNSIGNED_SHORT_5_6_5);
	use_texture_unit(UNIT_MD_WIDE);
	make_texture(GL_RGB8, SKY_WIDTH, LAYERS_HEIGHT, GL_RGB, GL_UNSIGNED_SHORT_5_6_5);
	use_texture_unit(UNIT_MD_BLENDED);
	GLuint sky_blended_texture = make_texture(GL_RGBA16F, SKY_WIDTH, LAYERS_HEIGHT, GL_RGBA, GL_FLOAT);
	use_texture_unit(UNIT_MD_FLAGS);
	make_texture(GL_R8UI, LAYERS_WIDTH, LAYERS_HEIGHT, GL_RED_INTEGER, GL_UNSIGNED_BYTE);
	use_texture_unit(UNIT_X32_INDICES);
	make_texture(GL_R8UI, LAYERS_WIDTH, LAYERS_HEIGHT, GL_RED_INTEGER, GL_UNSIGNED_BYTE);
	use_texture_unit(UNIT_X32_PALETTE);
	make_texture(GL_R16UI, 256, LAYERS_HEIGHT, GL_RED_INTEGER, GL_UNSIGNED_SHORT);
	use_texture_unit(UNIT_HLE_INDICES);
	GLuint index_texture = make_texture(GL_R8UI, HLE_IMAGE_WIDTH, HLE_IMAGE_HEIGHT, GL_RED_INTEGER, GL_UNSIGNED_BYTE);
	use_texture_unit(UNIT_HLE_COLOUR);
	hle_colour_texture = make_texture(GL_RGBA16F, HLE_IMAGE_WIDTH, HLE_IMAGE_HEIGHT, GL_RGBA, GL_FLOAT);
	use_texture_unit(UNIT_HLE_HAZE);
	hle_haze_texture = make_texture(GL_R16F, HLE_IMAGE_WIDTH, HLE_IMAGE_HEIGHT, GL_RED, GL_FLOAT);
	hle_colour_w = HLE_IMAGE_WIDTH;
	hle_colour_h = HLE_IMAGE_HEIGHT;
	use_texture_unit(UNIT_CORE_FRAME);          /* unit 0 stays active */
	make_texture(GL_RGB8, CORE_SCREEN_WIDTH, CORE_SCREEN_HEIGHT, GL_RGB, GL_UNSIGNED_SHORT_5_6_5);

	/* The HLE draws into its textures through framebuffers of their own */
	glGenFramebuffers(1, &hle_index_framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, hle_index_framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, index_texture, 0);
	glGenFramebuffers(1, &hle_colour_framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, hle_colour_framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hle_colour_texture, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, hle_haze_texture, 0);
	glDrawBuffers(2, HLE_OUTPUTS);
	glGenFramebuffers(1, &sky_framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, sky_framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sky_blended_texture, 0);
	/* MSAA: multisampled renderbuffers, sized when drawn into */
	glGetIntegerv(GL_MAX_SAMPLES, &msaa_samples);
	if (msaa_samples > MSAA_SAMPLES)
		msaa_samples = MSAA_SAMPLES;
	glGenFramebuffers(1, &msaa_framebuffer);
	glGenRenderbuffers(1, &msaa_renderbuffer);
	glGenRenderbuffers(1, &msaa_haze_renderbuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	hle_index_program = link_program(hle_vertex_shader, hle_index_shader);
	hle_colour_program = link_program(hle_vertex_shader, hle_colour_shader);
	glUseProgram(hle_colour_program);
	set_int(hle_colour_program, "x32_palette", UNIT_X32_PALETTE);
	colourize_program = link_program(quad_vertex_shader, colourize_shader);
	glUseProgram(colourize_program);
	set_int(colourize_program, "hle_indices", UNIT_HLE_INDICES);
	set_int(colourize_program, "x32_palette", UNIT_X32_PALETTE);
	set_vec2(colourize_program, "uv_scale", 1, 1);
	skyblend_program = link_program(quad_vertex_shader, skyblend_shader);
	glUseProgram(skyblend_program);
	set_int(skyblend_program, "md_wide", UNIT_MD_WIDE);
	set_vec2(skyblend_program, "uv_scale", 1, 1);
	glGenVertexArrays(1, &hle_vao);
	glBindVertexArray(hle_vao);
	glGenBuffers(1, &hle_vertex_buffer);
	glBindBuffer(GL_ARRAY_BUFFER, hle_vertex_buffer);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(hle_vertex_t), (void *)offsetof(hle_vertex_t, x));
	glEnableVertexAttribArray(1);
	glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(hle_vertex_t), (void *)offsetof(hle_vertex_t, index));
	glEnableVertexAttribArray(2);
	glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(hle_vertex_t), (void *)offsetof(hle_vertex_t, dithered));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(hle_vertex_t), (void *)offsetof(hle_vertex_t, depth));
	glUseProgram(blit_program);

	glGenVertexArrays(1, &quad_vao);   /* the core profile needs a bound VAO, even without attributes */
	glBindVertexArray(quad_vao);
}

/* ---- the HLE's 3D ---- */

/* Where exact vertices go, with the same measurement (best of 0-1 in quarters) */
#define EXACT_OFFSET_X 0.0f
#define EXACT_OFFSET_Y 1.0f

/* A polygon vertex in the image: the SH2's screen coordinates are relative to the centre
   (160, 112) with y up. The vertex goes to the centre of its pixel (+0.5): the GPU fills
   pixels whose centre is inside, the SH2s' spans run from where the edges cross each
   line, and this way the two agree best (94.7% of the pixels in a race, against 91.7%
   on the pixel's corner, measured with -t). */
static hle_vertex_t hle_vertex(const hle_polygon_t *polygon, int n)
{
	hle_vertex_t vertex;
	vertex.x = 160 + polygon->x[n] + 0.5f;
	vertex.y = 112 - polygon->y[n] + 0.5f;
	if (using_exact_vertices()) {
		/* The SH2 rounds down: its x and y are the exact ones less about 0.5 (and y points
		   up), so these land where the rounded ones do on average */
		vertex.x = 160 + polygon->fx[n] + EXACT_OFFSET_X;
		vertex.y = 112 - polygon->fy[n] + EXACT_OFFSET_Y;
	}
	vertex.index = polygon->colour >> 8;
	vertex.dithered = (polygon->colour & 2) != 0;
	vertex.depth = polygon->depth[n];
	return vertex;
}

/* How far exact polygons are pushed out: EXPAND pixels of the picture drawn, but at most
   EXPAND_MOST console pixels. In picture pixels because a fixed distance in console pixels
   makes thin polygons visibly fatter at high resolutions, and where two polygons overlap
   the one drawn later visibly eats into the other: a road's edge line then changes width
   from one track segment to the next. 0.75 closes the hairline cracks (below) as well as
   the SH2s' rounded vertices do (seams at 4x on frames 3500, 4400 and 6600 of the race
   script: 165, 24, 11, against 173, 40, 5 with the SH2s' vertices and 258, 36, 26 with
   0.375). At the console's resolution 0.375 makes the GPU's picture closest to the SH2s'
   (94.9% of the pixels, against 92.8% unexpanded and 94.7% with the SH2s' own vertices;
   measured with -d -t -f over 0.125-0.75). */
#define EXPAND      0.75f
#define EXPAND_MOST 0.375f

/* Moves a polygon's edges outwards by `distance` (console pixels), keeping their
   directions: each vertex goes to where its two edges' moved lines cross. Exact vertices
   leave hairline cracks where the models' neighbouring polygons do not quite meet (the
   SH2s' rounded vertices close them, and their fill covers more than the GPU's anyway).
   A sharp corner, where the lines cross more than 2 * distance away, is cut off instead:
   it becomes two vertices, one on each moved edge, so that the edges stay straight.
   Writes up to 2 * count vertices to out and returns how many; degenerate polygons stay
   as they are. */
static int expand_polygon(const hle_vertex_t *v, int count, float distance, hle_vertex_t *out)
{
	float area = 0;
	for (int i = 0; i < count; i++) {
		const hle_vertex_t *a = &v[i], *b = &v[(i + 1) % count];
		area += a->x * b->y - b->x * a->y;
	}
	if (fabsf(area) < 1e-3f) {
		memcpy(out, v, count * sizeof(*v));
		return count;
	}
	float outwards = area > 0 ? 1 : -1;   /* (dy, -dx) points out of a positive-area polygon */

	float normal_x[4], normal_y[4];       /* each edge's outward unit normal */
	for (int i = 0; i < count; i++) {
		const hle_vertex_t *a = &v[i], *b = &v[(i + 1) % count];
		float dx = b->x - a->x, dy = b->y - a->y, length = sqrtf(dx * dx + dy * dy);
		normal_x[i] = length > 0 ? outwards * dy / length : 0;
		normal_y[i] = length > 0 ? -outwards * dx / length : 0;
	}
	int moved = 0;
	for (int i = 0; i < count; i++) {
		int before = (i + count - 1) % count;
		float dot = 1 + normal_x[before] * normal_x[i] + normal_y[before] * normal_y[i];
		if (dot > 0.5f) {
			/* the corner's offset m solves m . n1 = m . n2 = distance */
			float scale = distance / dot;
			out[moved] = v[i];
			out[moved].x += (normal_x[before] + normal_x[i]) * scale;
			out[moved].y += (normal_y[before] + normal_y[i]) * scale;
			moved++;
			continue;
		}
		out[moved] = v[i];
		out[moved].x += normal_x[before] * distance;
		out[moved].y += normal_y[before] * distance;
		moved++;
		out[moved] = v[i];
		out[moved].x += normal_x[i] * distance;
		out[moved].y += normal_y[i] * distance;
		moved++;
	}
	return moved;
}

/* How far a HUD polygon the HLE draws (the needle, the course map and its dots) moves:
   with the group of where its model is placed, the whole model together. Moved polygon by
   polygon, the course map came apart when it reached left of column 180 (it is centred on
   the player's car and turns with the view: Highland, a paused replay). */
static int hud_polygon_shift(const hle_polygon_t *polygon)
{
	int anchor = 160 + polygon->hud_anchor;
	return hud_shift(group_of_span(anchor, anchor));
}

/* The polygon list as triangles for the GPU, exact polygons pushed out by `expand` console
   pixels; returns the number of vertices */
static int hle_triangles(const hle_frame_t *frame, hle_vertex_t *vertices, float expand)
{
	int count = 0;
	for (int i = 0; i < frame->count; i++) {
		const hle_polygon_t *polygon = &frame->polygons[i];
		if (polygon->drawn_with == (using_exact_vertices() ? HLE_SH2_VERTICES_ONLY : HLE_EXACT_VERTICES_ONLY))
			continue;   /* faces away with these vertices */
		hle_vertex_t corners[4], expanded[8];
		int corner_count = polygon->vertex_count;
		for (int n = 0; n < corner_count; n++)
			corners[n] = hle_vertex(polygon, n);
		if (i >= frame->hud_first && moving_hud) {
			int shift = hud_polygon_shift(polygon);
			for (int n = 0; n < corner_count; n++)
				corners[n].x += shift;
		}
		const hle_vertex_t *fan = corners;
		if (using_exact_vertices()) {
			corner_count = expand_polygon(corners, corner_count, expand, expanded);
			fan = expanded;
		}
		/* a fan of triangles: 0-1-2, 0-2-3, ... */
		for (int corner = 2; corner < corner_count; corner++) {
			vertices[count++] = fan[0];
			vertices[count++] = fan[corner - 1];
			vertices[count++] = fan[corner];
		}
	}
	return count;
}

/* Whether a widescreen picture fills the black borders above and below the 3D with more
   3D and sky: when the 3D has its usual lines (races and the demo; the demo narrows them
   as it closes) */
#define FULL_VIEW_TOP    16
#define FULL_VIEW_BOTTOM 207

static int fills_borders(const hle_frame_t *frame)
{
	return widescreen && frame->view_top == FULL_VIEW_TOP && frame->view_bottom == FULL_VIEW_BOTTOM;
}

/* Draws a polygon list with a program into the bound w x h framebuffer, the whole
   picture (picture_width() console columns), inside the list's 3D view (rows 16-207 in a
   race, scaled), or all rows when filling the borders */
static void hle_draw(const hle_frame_t *frame, GLuint program, int w, int h)
{
	static hle_vertex_t vertices[HLE_MAX_POLYGONS * 18];   /* up to 8 corners: 6 triangles */
	float scale_x = (float)w / picture_width(), scale_y = (float)h / HLE_IMAGE_HEIGHT;
	int count = hle_triangles(frame, vertices, fminf(EXPAND / scale_x, EXPAND_MOST));

	glViewport(0, 0, w, h);
	glEnable(GL_SCISSOR_TEST);
	int top = (int)floorf(frame->view_top * scale_y);
	int bottom = (int)ceilf((frame->view_bottom + 1) * scale_y);
	if (fills_borders(frame)) {
		top = 0;
		bottom = h;
	}
	glScissor(0, top, w, bottom - top);

	glUseProgram(program);
	set_int(program, "picodrive_green", picodrive_green);
	set_int(program, "blend_dither", blend_dither);
	set_int(program, "haze", far_view == FAR_VIEW_HAZE);
	float haze_end = haze_end_units();
	set_float(program, "haze_start", fminf(haze_start * 1000.0f, haze_end - 1));
	set_float(program, "haze_end", haze_end);
	set_vec2(program, "scale", scale_x, scale_y);
	set_vec2(program, "image_size", w, h);
	set_float(program, "side", (float)picture_side());
	glBindVertexArray(hle_vao);
	glBindBuffer(GL_ARRAY_BUFFER, hle_vertex_buffer);
	glBufferData(GL_ARRAY_BUFFER, count * sizeof(hle_vertex_t), vertices, GL_STREAM_DRAW);
	glDrawArrays(GL_TRIANGLES, 0, count);

	glDisable(GL_SCISSOR_TEST);
	glBindVertexArray(quad_vao);
	glUseProgram(blit_program);
}

/* The -t checks: palette indices at the console's resolution, into UNIT_HLE_INDICES */
static void hle_draw_indices(const hle_frame_t *frame)
{
	GLint previous_framebuffer;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, hle_index_framebuffer);
	GLuint nothing[4] = { 0, 0, 0, 0 };
	glClearBufferuiv(GL_COLOR, 0, nothing);
	hle_draw(frame, hle_index_program, HLE_IMAGE_WIDTH, HLE_IMAGE_HEIGHT);
	glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);
}

/* The index texture, read back: row 0 is the screen's top line */
static void hle_read_indices(uint8_t *image)
{
	GLint previous_framebuffer;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, hle_index_framebuffer);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, HLE_IMAGE_WIDTH, HLE_IMAGE_HEIGHT, GL_RED_INTEGER, GL_UNSIGNED_BYTE, image);
	glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);
}

/* The colour texture at w x h */
static void size_colour_texture(int w, int h)
{
	if (w == hle_colour_w && h == hle_colour_h)
		return;
	use_texture_unit(UNIT_HLE_COLOUR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, NULL);
	use_texture_unit(UNIT_HLE_HAZE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, w, h, 0, GL_RED, GL_FLOAT, NULL);
	use_texture_unit(UNIT_CORE_FRAME);
	hle_colour_w = w;
	hle_colour_h = h;
}

/* The MSAA renderbuffer at w x h; returns 0 if the GL cannot do it */
static int size_msaa(int w, int h)
{
	if (msaa_samples < 2)
		return 0;
	if (w == msaa_w && h == msaa_h)
		return 1;
	glBindRenderbuffer(GL_RENDERBUFFER, msaa_renderbuffer);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaa_samples, GL_RGBA16F, w, h);
	glBindRenderbuffer(GL_RENDERBUFFER, msaa_haze_renderbuffer);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaa_samples, GL_R16F, w, h);
	glBindFramebuffer(GL_FRAMEBUFFER, msaa_framebuffer);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msaa_renderbuffer);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_RENDERBUFFER, msaa_haze_renderbuffer);
	glDrawBuffers(2, HLE_OUTPUTS);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "MSAA: %d samples of RGBA16F not supported, drawing without\n", msaa_samples);
		msaa_samples = 0;
		return 0;
	}
	msaa_w = w;
	msaa_h = h;
	return 1;
}

/* The 3D as shown, at w x h, into the colour texture; with MSAA drawn into the
   multisampled renderbuffer first and resolved (each pixel the mean of its samples,
   so edges get partial coverage) */
static void hle_draw_colour(const hle_frame_t *frame, int w, int h, int multisample)
{
	GLint previous_framebuffer;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
	size_colour_texture(w, h);
	multisample = multisample && size_msaa(w, h);

	glBindFramebuffer(GL_FRAMEBUFFER, multisample ? msaa_framebuffer : hle_colour_framebuffer);
	glViewport(0, 0, w, h);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   /* premultiplied: "over" */
	hle_draw(frame, hle_colour_program, w, h);
	glDisable(GL_BLEND);
	if (multisample) {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, msaa_framebuffer);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, hle_colour_framebuffer);
		for (int i = 0; i < 2; i++) {   /* a blit copies one buffer: the colour, then the haze */
			glReadBuffer(HLE_OUTPUTS[i]);
			glDrawBuffers(1, &HLE_OUTPUTS[i]);
			glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		}
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		glDrawBuffers(2, HLE_OUTPUTS);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);
}

/* -t: the index texture into the colour texture, at the console's resolution */
static void hle_colourize(void)
{
	GLint previous_framebuffer;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
	size_colour_texture(HLE_IMAGE_WIDTH, HLE_IMAGE_HEIGHT);
	glBindFramebuffer(GL_FRAMEBUFFER, hle_colour_framebuffer);
	glViewport(0, 0, HLE_IMAGE_WIDTH, HLE_IMAGE_HEIGHT);
	glUseProgram(colourize_program);
	set_int(colourize_program, "picodrive_green", picodrive_green);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glUseProgram(blit_program);
	glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);
}

/* ---- uploading an emulated frame ---- */

/* Whether the frame buffer keeps a whole line (hud_keep_framebuffer_pixel) */
static int line_kept(int row)
{
	for (int x = 0; x < LAYERS_WIDTH; x++)
		if (!layers.x32_keep[row][x])
			return 0;
	return 1;
}

/* Whether some line of the 3D view is kept whole this frame: a blinds transition is on,
   and the borders stay black as in the original */
static int in_transition;

/* MD_FLAG_* for each visible pixel */
static void compute_md_flags(uint8_t flags[LAYERS_HEIGHT][LAYERS_WIDTH])
{
	in_transition = 0;
	for (int y = 0; y < frame_height(); y++) {
		int row = core_visible_top + y;
		int kept = layers.x32_shown[row] && line_kept(row);
		if (kept && y >= FULL_VIEW_TOP && y <= FULL_VIEW_BOTTOM)
			in_transition = 1;
		for (int x = 0; x < LAYERS_WIDTH; x++) {
			if (!layers.x32_shown[row])
				flags[y][x] = MD_FLAG_MD_ONLY;
			else if (layers.md_index[row][x] == layers.md_background[row])
				flags[y][x] = MD_FLAG_BACKGROUND;
			else
				flags[y][x] = MD_FLAG_PICTURE;
			if (layers.x32_keep[row][x])
				flags[y][x] |= MD_FLAG_KEEP_32X;
			if (kept)
				flags[y][x] |= MD_FLAG_LINE_KEPT;
			if (moving_hud)
				flags[y][x] |= hud_group[layers.x32_hud_item[row][x]] << MD_FLAG_HUD_SHIFT;
		}
	}
}

/* Rows of `height` from `first_row` of an array of rows `row_length` elements long */
static void upload_rows(int unit, const void *first_row, int row_length, int width, int height,
	GLenum format, GLenum type)
{
	use_texture_unit(unit);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, first_row);
	use_texture_unit(UNIT_CORE_FRAME);
}

/* In a race the Mega Drive layer is plane B alone, so the middle of plane B as we draw
   it (layers.c) must be PicoDrive's MD picture, in the 3D view; it is not as the race
   comes on and goes (the VDP is set up anew in mid-frame). Returns the number of pixels
   that differ. */
static int check_plane_b(const hle_frame_t *frame)
{
	int different = 0;
	for (int line = frame->view_top; line <= frame->view_bottom; line++) {
		int row = core_visible_top + line;
		for (int x = 0; x < LAYERS_WIDTH; x++)
			different += layers.md_wide[row][SKY_MARGIN + x] != layers.md_picture[row][x];
	}
	return different;
}

/* Whether this frame's plane B as we draw it can stand for the MD layer (check_plane_b):
   only then the sky is moved or blended */
static int sky_ready;

/* Whether a widescreen picture's sides can repeat its edges (repeat_edge), as for a
   picture wholly of the 32X layer with a pattern or plain colour at the edges (the
   title's checks, the SEGA logo): where both strips of EDGE_REPEAT columns show the 32X
   layer on every line, the same every PATTERN_PERIOD columns. Screens with the Mega Drive
   layer at the edges (the menus) keep black sides, their panels and text would show
   twice; so do pictures with something else at the edges (the big "V.R." as the attract
   loop starts, whose full stop would show over and over). */
static int sides_wrap;
#define PATTERN_PERIOD 16   /* the title's checks: 8 pixels each way */

static int edge_shows_32x(int row, int x)
{
	int md_is_background = layers.md_index[row][x] == layers.md_background[row];
	int x32_has_priority = (layers.x32_palette[row][layers.x32_index[row][x]] & 0x8000) != 0;
	return md_is_background || x32_has_priority;
}

static int edges_repeat(void)
{
	for (int y = 0; y < frame_height(); y++) {
		int row = core_visible_top + y;
		if (!layers.x32_shown[row])
			return 0;
		const uint8_t *index = layers.x32_index[row];
		for (int i = 0; i < EDGE_REPEAT; i++) {
			int left = i, right = LAYERS_WIDTH - EDGE_REPEAT + i;
			if (!edge_shows_32x(row, left) || !edge_shows_32x(row, right))
				return 0;
			if (i + PATTERN_PERIOD < EDGE_REPEAT
			    && (index[left] != index[left + PATTERN_PERIOD] || index[right] != index[right + PATTERN_PERIOD]))
				return 0;
		}
	}
	return 1;
}

/* After an emulated frame: the core's frame and our two layers, visible rows only */
/* The sky's dither blended (skyblend_shader) from the first h rows of UNIT_MD_WIDE */
static void blend_sky(int h)
{
	GLint previous_framebuffer;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, sky_framebuffer);
	glViewport(0, 0, SKY_WIDTH, h);
	glUseProgram(skyblend_program);
	glUniform2i(glGetUniformLocation(skyblend_program, "size"), SKY_WIDTH, h);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glUseProgram(blit_program);
	glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);
}

static void upload_frame(void)
{
	moving_hud = widescreen && hle_visible != NULL && hle_visible->command == HLE_RACE_COMMAND
		&& (layers.x32_hud_types & AFTER_RACE_TYPES) == 0;
	group_hud_items();
	static uint8_t md_flags[LAYERS_HEIGHT][LAYERS_WIDTH];
	int top = core_visible_top;
	int w = frame_width(), h = frame_height();

	compute_md_flags(md_flags);
	upload_rows(UNIT_CORE_FRAME, &core_screen[top * CORE_SCREEN_WIDTH], CORE_SCREEN_WIDTH, w, h,
		GL_RGB, GL_UNSIGNED_SHORT_5_6_5);
	upload_rows(UNIT_MD_PICTURE, layers.md_picture[top], LAYERS_WIDTH, w, h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5);
	sky_ready = hle_visible != NULL && check_plane_b(hle_visible) == 0;
	sides_wrap = hle_visible == NULL && widescreen && edges_repeat();
	if (sky_ready) {
		upload_rows(UNIT_MD_WIDE, layers.md_wide[top], SKY_WIDTH, SKY_WIDTH, h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5);
		if (blend_dither)
			blend_sky(h);
	}
	upload_rows(UNIT_MD_FLAGS, md_flags, LAYERS_WIDTH, w, h, GL_RED_INTEGER, GL_UNSIGNED_BYTE);
	upload_rows(UNIT_X32_INDICES, layers.x32_index[top], LAYERS_WIDTH, w, h, GL_RED_INTEGER, GL_UNSIGNED_BYTE);
	upload_rows(UNIT_X32_PALETTE, layers.x32_palette[top], 256, 256, h, GL_RED_INTEGER, GL_UNSIGNED_SHORT);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

/* ---- drawing to the screen ---- */

/* The core's frame as a textured quad */
static void blit(int x, int y, int w, int h)
{
	set_vec2(blit_program, "uv_scale", (float)frame_width() / CORE_SCREEN_WIDTH,
		(float)frame_height() / CORE_SCREEN_HEIGHT);
	glViewport(x, y, w, h);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* Our composite of the two layers, with this polygon list's 3D (drawn into the colour
   texture by hle_draw_colour) unless NULL; in a race, the sky moved by sky_offset pixels
   to the left (plane B as we draw it, with the dither blended if asked) */
static void composite(int x, int y, int w, int h, const hle_frame_t *hle, double sky_offset)
{
	glUseProgram(composite_program);
	set_int(composite_program, "picodrive_green", picodrive_green);
	/* in a race the Mega Drive layer is plane B alone: the sky (docs/screen-layers*) */
	int use_sky = sky_ready && (blend_dither || sky_offset != 0 || widescreen);
	set_int(composite_program, "use_sky", use_sky);
	set_float(composite_program, "sky_offset", (float)sky_offset);
	set_int(composite_program, "use_md_blended", use_sky && blend_dither);
	set_vec2(composite_program, "frame_size", picture_width(), frame_height());
	glUniform1i(glGetUniformLocation(composite_program, "side"), picture_side());
	set_int(composite_program, "fill_borders", hle != NULL && use_sky && fills_borders(hle) && !in_transition);
	set_int(composite_program, "wrap_sides", widescreen && hle == NULL && sides_wrap);
	set_int(composite_program, "move_hud", hle != NULL && moving_hud);
	glUniform2i(glGetUniformLocation(composite_program, "sky_lines"), layers.md_wide_first_line,
		layers.md_wide_first_line + layers.md_wide_lines - 1);
	set_int(composite_program, "use_hle", hle != NULL);
	set_int(composite_program, "use_haze", hle != NULL && far_view == FAR_VIEW_HAZE);
	set_vec2(composite_program, "hle_size", hle_colour_w, hle_colour_h);
	if (hle)
		glUniform2i(glGetUniformLocation(composite_program, "hle_lines"), hle->view_top, hle->view_bottom);
	glViewport(x, y, w, h);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glUseProgram(blit_program);
}

/* Where a picture `width` console pixels wide goes in the area x0..x0+w by h:
   letterboxed, at the TV's pixel aspect. rect gets x, y, width, height. */
static void fit(int width, int x0, int w, int h, int rect[4])
{
	double aspect = width * PIXEL_ASPECT / frame_height();
	int fitted_w = w, fitted_h = h;
	if ((double)w / h > aspect)
		fitted_w = (int)lround(h * aspect);
	else
		fitted_h = (int)lround(w / aspect);
	rect[0] = x0 + (w - fitted_w) / 2;
	rect[1] = (h - fitted_h) / 2;
	rect[2] = fitted_w;
	rect[3] = fitted_h;
}

/* How far the display is between emulated frames (0 to 1): with a frame every
   frames_per_vsync refreshes, which refresh of those this is; otherwise the time left over
   after the last frame, in frames */
static double frame_fraction(int frames_per_vsync, long periods, long next_frame_period, double time_left_over)
{
	double fraction = frames_per_vsync
		? 1.0 - (double)(next_frame_period - periods) / frames_per_vsync
		: time_left_over;
	return fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
}

/* A difference of plane B scrolls (10 bits) the shorter way round */
static double scroll_difference(double difference)
{
	difference = fmod(difference, 1024.0);
	if (difference >= 512)
		difference -= 1024;
	if (difference < -512)
		difference += 1024;
	return difference;
}

/* The game scrolls plane B, the sky, once a tick, in the frame it sends the scene
   (docs/game-loop.zh-TW.md): the scrolls that came with the last few scenes, by
   hle_scene_count(). A few, not two: at 30 or 40 images a second the image of the pair
   before can stay a little after the next scene (hle_interpolated), and its sky has to
   stay with it. */
#define SKY_SCROLLS_KEPT 4
/* The sky moves at most this far (sky_offset_for); plane B as drawn covers it next to the
   widest picture */
#define SKY_OFFSET_LIMIT 102
_Static_assert(SKY_MARGIN >= WIDEST_SIDE + SKY_OFFSET_LIMIT, "plane B drawn too narrow");
static int sky_scrolls[SKY_SCROLLS_KEPT];
static int scenes_seen;

/* After each emulated frame: a scene sent in it came with the scroll just written */
static void note_sky_scroll(void)
{
	if (hle_scene_count() == scenes_seen)
		return;
	scenes_seen = hle_scene_count();
	sky_scrolls[scenes_seen % SKY_SCROLLS_KEPT] = layers_plane_b_scroll();
}

/* With smooth motion the sky's scroll goes from the older scene's to the newer's as the
   camera does, for the pair of scenes the image was built from: how many pixels to the
   left of where this frame's plane B is */
static double sky_offset_for(const hle_frame_t *frame)
{
	if (scenes_seen - frame->scene > SKY_SCROLLS_KEPT - 2 || frame->scene < 1)
		return 0;
	int older = sky_scrolls[(frame->scene - 1) % SKY_SCROLLS_KEPT];
	int newer = sky_scrolls[frame->scene % SKY_SCROLLS_KEPT];
	double moved = scroll_difference(newer - older) * frame->camera_t;
	double offset = scroll_difference(layers.md_wide_scroll - (older + moved));
	/* not one tick's turn (at most 65 pixels in a race) */
	return fabs(offset) < SKY_OFFSET_LIMIT ? offset : 0;
}

/* With smooth motion at 30, 40 or 60 images a second, the emulated frames each image
   stays (hle_interpolated); 0 at the display's rate or without smooth motion */
static double frames_per_image(void)
{
	return smooth == SMOOTH_30 ? 2 : smooth == SMOOTH_40 ? 1.5 : smooth == SMOOTH_60 ? 1 : 0;
}

/* The race picture on screen, or with smooth motion the scenes' state at this moment
   (`fraction` of a frame after the last emulated one) and how far to move the sky; NULL
   when the screen shows no race picture */
static const hle_frame_t *hle_to_draw(double fraction, double *sky_offset)
{
	*sky_offset = 0;
	if (hle_visible == NULL)
		return NULL;
	const hle_frame_t *between = smooth ? hle_interpolated(fraction, hle_visible, frames_per_image()) : NULL;
	if (between == NULL)
		return hle_visible;
	*sky_offset = sky_offset_for(between);
	return between;
}

/* Since the title was last updated: the 3D pictures drawn, the motion as set in a race
   (20 a second without smooth motion, 30, 40, 60 or every refresh with it), and the frames
   emulated, each a new picture from the console. A picture counts even when it is the
   same as the last one (a still scene), so a still scene does not look like a slow one. */
static int new_3d_pictures, new_emulated_frames;
static long emulated_frames, race_pictures;   /* since start: run_frame's, and the new race
                                                 pictures among them (one a tick) */

/* Count the 3D picture about to be drawn, `fraction` of a frame after the last emulated
   one, if it is a new one: each refresh at the display's rate, each step at 30, 40 or 60
   a second (as hle_interpolated takes them), each tick without smooth motion */
static void count_3d_picture(double fraction)
{
	static double last = -1;
	double picture;
	if (smooth == SMOOTH_DISPLAY) {
		new_3d_pictures++;
		return;
	}
	if (smooth)
		picture = floor((emulated_frames + fraction) / frames_per_image() + 1e-9);
	else
		picture = (double)race_pictures;
	if (picture != last) {
		last = picture;
		new_3d_pictures++;
	}
}

/* The far view looks as far to the side as the picture shows: 2 times the depth for 16:9
   (whose edges are at 1.95), more in proportion for a wider picture */
static void set_side_reach(void)
{
	static int reach_set = HLE_SIDE_REACH_16_9;
	int reach = (int)ceil(HLE_SIDE_REACH_16_9 * picture_width() / 436.0);
	if (reach < HLE_SIDE_REACH_16_9)
		reach = HLE_SIDE_REACH_16_9;
	if (reach != reach_set)
		hle_set_side_reach(reach_set = reach);
}

/* One display refresh into a w x h window, `fraction` of a frame after the last
   emulated one */
static int drawn_3d_w, drawn_3d_h;   /* the 3D's size in the last picture, 0 without 3D */

static void draw(int w, int h, double fraction)
{
	int original_rect[4], composite_rect[4];
	/* side by side, our picture has the right half */
	int area_w = view == VIEW_SIDE_BY_SIDE ? w - w / 2 : w;
	area_aspect = (double)area_w / h;
	set_side_reach();
	if (view == VIEW_SIDE_BY_SIDE) {
		fit(frame_width(), 0, w / 2, h, original_rect);
		fit(picture_width(), w / 2, w - w / 2, h, composite_rect);
	} else {
		fit(frame_width(), 0, w, h, original_rect);
		fit(picture_width(), 0, w, h, composite_rect);
	}

	double sky_offset = 0;
	const hle_frame_t *hle = NULL;
	if (hle_on && view != VIEW_ORIGINAL)
		hle = hle_to_draw(fraction, &sky_offset);
	if (hle && !menu.open)
		count_3d_picture(fraction);
	if (hle) {
		/* the 3D at the chosen resolution, or the picture's size on screen (at least the
		   console's) */
		int hle_w = resolution * picture_width(), hle_h = resolution * HLE_IMAGE_HEIGHT;
		if (resolution == RESOLUTION_WINDOW) {
			hle_w = composite_rect[2] > picture_width() ? composite_rect[2] : picture_width();
			hle_h = composite_rect[3] > HLE_IMAGE_HEIGHT ? composite_rect[3] : HLE_IMAGE_HEIGHT;
		}
		hle_draw_colour(hle, hle_w, hle_h, msaa);
		drawn_3d_w = hle_w;
		drawn_3d_h = hle_h;
	} else {
		drawn_3d_w = drawn_3d_h = 0;
	}

	glViewport(0, 0, w, h);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	if (view != VIEW_COMPOSITE)
		blit(original_rect[0], original_rect[1], original_rect[2], original_rect[3]);
	if (view != VIEW_ORIGINAL)
		composite(composite_rect[0], composite_rect[1], composite_rect[2], composite_rect[3], hle, sky_offset);
}

/* ---- input ---- */

/* The first game controller. SDL names its buttons by their places on an Xbox pad (A
   bottom, B right, X left, Y top), so PlayStation and Switch Pro pads map the same way:
   the face buttons sit like the Mega Drive's, A B C along the bottom row and Z on top.
   (Nintendo pads need SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS off, set in main, or SDL
   names their buttons by their labels, which are swapped.) */
static SDL_GameController *controller;

/* How far a stick or trigger must go, of 32767, to count as pressed: the game's steering
   is digital, so the left stick works as the D-pad. */
#define AXIS_PRESSED 12000

static const struct {
	SDL_GameControllerButton pad_button;
	int button;
} controller_buttons[] = {
	{ SDL_CONTROLLER_BUTTON_DPAD_UP,       GBTN_UP    },
	{ SDL_CONTROLLER_BUTTON_DPAD_DOWN,     GBTN_DOWN  },
	{ SDL_CONTROLLER_BUTTON_DPAD_LEFT,     GBTN_LEFT  },
	{ SDL_CONTROLLER_BUTTON_DPAD_RIGHT,    GBTN_RIGHT },
	{ SDL_CONTROLLER_BUTTON_X,             GBTN_A     },
	{ SDL_CONTROLLER_BUTTON_A,             GBTN_B     },   /* gas in a race */
	{ SDL_CONTROLLER_BUTTON_B,             GBTN_C     },
	{ SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  GBTN_X     },   /* X, Y, Z pick the camera view */
	{ SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, GBTN_Y     },
	{ SDL_CONTROLLER_BUTTON_Y,             GBTN_Z     },
	{ SDL_CONTROLLER_BUTTON_START,         GBTN_START },
};

static const struct {
	SDL_Scancode key;
	int button;
} keyboard_buttons[] = {
	{ SDL_SCANCODE_UP,    GBTN_UP    },
	{ SDL_SCANCODE_DOWN,  GBTN_DOWN  },
	{ SDL_SCANCODE_LEFT,  GBTN_LEFT  },
	{ SDL_SCANCODE_RIGHT, GBTN_RIGHT },
	{ SDL_SCANCODE_A,     GBTN_A     },
	{ SDL_SCANCODE_S,     GBTN_B     },   /* gas in a race */
	{ SDL_SCANCODE_D,     GBTN_C     },
	{ SDL_SCANCODE_F,     GBTN_START },
	{ SDL_SCANCODE_Z,     GBTN_X     },   /* X, Y, Z pick the camera view */
	{ SDL_SCANCODE_X,     GBTN_Y     },
	{ SDL_SCANCODE_C,     GBTN_Z     },
	{ SDL_SCANCODE_V,     GBTN_MODE  },
};

static int axis_pressed(SDL_GameControllerAxis axis, int direction)
{
	return SDL_GameControllerGetAxis(controller, axis) * direction > AXIS_PRESSED;
}

/* Opens a controller when one is plugged in and drops it when it is pulled out */
static uint16_t read_controller(void)
{
	uint16_t buttons = 0;
	if (controller && !SDL_GameControllerGetAttached(controller)) {
		SDL_GameControllerClose(controller);
		controller = NULL;
	}
	for (int i = 0; !controller && i < SDL_NumJoysticks(); i++) {
		if (SDL_IsGameController(i)) {
			controller = SDL_GameControllerOpen(i);
			if (controller)
				fprintf(stderr, "controller: %s (SDL type %d)\n", SDL_GameControllerName(controller),
				        (int)SDL_GameControllerGetType(controller));
		}
	}
	if (!controller)
		return 0;

	for (size_t i = 0; i < SDL_arraysize(controller_buttons); i++) {
		if (SDL_GameControllerGetButton(controller, controller_buttons[i].pad_button))
			buttons |= 1 << controller_buttons[i].button;
	}
	if (axis_pressed(SDL_CONTROLLER_AXIS_LEFTY, -1)) buttons |= 1 << GBTN_UP;
	if (axis_pressed(SDL_CONTROLLER_AXIS_LEFTY, 1)) buttons |= 1 << GBTN_DOWN;
	if (axis_pressed(SDL_CONTROLLER_AXIS_LEFTX, -1)) buttons |= 1 << GBTN_LEFT;
	if (axis_pressed(SDL_CONTROLLER_AXIS_LEFTX, 1)) buttons |= 1 << GBTN_RIGHT;
	/* with analog on, the triggers are the pedals only (read_analog): as buttons too, past
	   their AXIS_PRESSED a trigger would also set off the game's own checks of the buttons */
	if (!analog && axis_pressed(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1)) buttons |= 1 << GBTN_A;
	if (!analog && axis_pressed(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1)) buttons |= 1 << GBTN_B;
	return buttons;
}

/* With analog on, the left stick's steering target and how far the triggers press the
   throttle (right) and the brake (left), as analog.h has them; 0 = the buttons work it.
   Kept with the stick's axis for the debug info. */
static struct {
	int stick_axis;
	int steering, throttle, brake;
} analog_input;

static void read_analog(void)
{
	SDL_GameController *pad = controller;
	analog_input.stick_axis = pad ? SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) : 0;
	analog_input.steering = analog_input.throttle = analog_input.brake = 0;
	if (!analog || !pad || menu.wait_for_release)
		return;
	analog_input.steering = analog_stick_target(analog_input.stick_axis, stick_dead_zone);
	analog_input.throttle = analog_trigger_amount(
		SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT), trigger_dead_zone);
	analog_input.brake = analog_trigger_amount(
		SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT), trigger_dead_zone);
}

/* The keyboard and the controller together */
static uint16_t read_pad(void)
{
	const Uint8 *key_is_down = SDL_GetKeyboardState(NULL);
	uint16_t buttons = read_controller();
	for (size_t i = 0; i < SDL_arraysize(keyboard_buttons); i++) {
		if (key_is_down[keyboard_buttons[i].key])
			buttons |= 1 << keyboard_buttons[i].button;
	}
	/* a real D-pad cannot press opposite directions at once */
	if (buttons & (1 << GBTN_UP))
		buttons &= ~(1 << GBTN_DOWN);
	if (buttons & (1 << GBTN_LEFT))
		buttons &= ~(1 << GBTN_RIGHT);
	/* the button that closed the menu does not reach the game */
	if (menu.wait_for_release) {
		if (buttons)
			return 0;
		menu.wait_for_release = 0;
	}
	return buttons;
}

/* The core's hooks (pico.h): the HLE follows the 68000's commands and scenes */
static void on_comm_write(int writer, unsigned int pc, unsigned int offset, unsigned int value, int size)
{
	hle_on_comm_write(writer, offset, value, size);
}

static void on_dreq_write(int is_slave, unsigned int address, unsigned int value)
{
	hle_on_dreq_write(address);
}

/* Which pixels of the frame buffer are the HUD graphics the master SH2 copies in */
static void on_sh2_access(int is_slave, unsigned int pc, unsigned int address, unsigned int value,
	int size, int is_write)
{
	hud_note_sh2_access(is_slave, address, value, size, is_write);
}

/* One emulated frame, keeping the layers. The frame buffer the 32X shows in this frame
   was chosen at the end of the previous one, and so was its polygon list. Returns the
   polygon list of a race picture that has just come into the frame buffer on screen
   (from the next frame on), or NULL. */
static const hle_frame_t *run_frame(uint16_t buttons)
{
	hle_visible = hle_shown();
	layers_frame_start();
	core_run_frame(buttons);
	layers_frame_end();
	note_sky_scroll();
	const hle_frame_t *new_picture = hle_on_frame_end();
	emulated_frames++;
	if (new_picture)
		race_pictures++;
	return new_picture;
}

/* ---- -d: checks and images ---- */

static void rgb565_to_rgb888(uint16_t colour, unsigned char *rgb)
{
	rgb[0] = (colour >> 11) << 3;
	rgb[1] = ((colour >> 5) & 63) << 2;
	rgb[2] = (colour & 31) << 3;
}

/* GL's RGBA8 readback of an RGB565 colour, back to RGB565 */
static uint16_t rgba8_to_rgb565(const unsigned char *rgba)
{
	return ((rgba[0] * 31 + 127) / 255) << 11 | ((rgba[1] * 63 + 127) / 255) << 5 | ((rgba[2] * 31 + 127) / 255);
}

/* The w x h image at the bottom left of the bound framebuffer, top-down, as RGB565 */
static void read_rgb565(int w, int h, uint16_t *out)
{
	static unsigned char rgba[LAYERS_WIDTH * LAYERS_HEIGHT * 4];
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	for (int y = 0; y < h; y++) {
		const unsigned char *gl_row = rgba + (h - 1 - y) * w * 4;   /* GL rows are bottom-up */
		for (int x = 0; x < w; x++)
			out[y * w + x] = rgba8_to_rgb565(gl_row + x * 4);
	}
}

static void write_rgb565_ppm(const char *path, int w, int h, const uint16_t *pixels, int row_length)
{
	FILE *file = fopen(path, "wb");
	if (!file) {
		perror(path);
		return;
	}
	fprintf(file, "P6\n%d %d\n255\n", w, h);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			unsigned char rgb[3];
			rgb565_to_rgb888(pixels[y * row_length + x], rgb);
			fwrite(rgb, 1, 3, file);
		}
	}
	fclose(file);
	printf("wrote %s (%dx%d)\n", path, w, h);
}

/* A framebuffer with a w x h RGBA8 renderbuffer, left bound */
static GLuint create_offscreen_framebuffer(int w, int h)
{
	GLuint framebuffer, renderbuffer;
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glGenRenderbuffers(1, &renderbuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
	return framebuffer;
}

/* Our composite (drawn into the bound framebuffer) against the core's frame; returns the
   number of rows that differ, and reports the first difference of the run */
static int check_composite(int frame, uint16_t *ours, int report_first)
{
	int w = frame_width(), h = frame_height(), bad_rows = 0;
	composite(0, 0, w, h, NULL, 0);
	read_rgb565(w, h, ours);
	for (int y = 0; y < h; y++) {
		const uint16_t *core_row = &core_screen[(core_visible_top + y) * CORE_SCREEN_WIDTH];
		int x = 0;
		while (x < w && core_row[x] == ours[y * w + x])
			x++;
		if (x == w)
			continue;
		if (bad_rows == 0 && report_first)
			printf("composite: first difference at frame %d line %d x %d\n", frame, y, x);
		bad_rows++;
	}
	return bad_rows;
}

/* -t: how the GPU's HLE pictures compare with the SH2s' */
#define HLE_LOW_MATCH 85.0   /* pictures below this are listed */
static struct {
	int pictures, low;
	double sum, lowest;
	int lowest_frame;
} gpu_results = { 0, 0, 0, 100.0, -1 };

static void check_hle_gpu(int frame, const hle_frame_t *picture)
{
	static uint8_t image[HLE_IMAGE_WIDTH * HLE_IMAGE_HEIGHT];
	hle_draw_indices(picture);
	hle_read_indices(image);
	int compared, same;
	hlecheck_compare(picture, image, &compared, &same);
	if (compared == 0)
		return;   /* nothing drawn on either side */

	double percent = 100.0 * same / compared;
	gpu_results.pictures++;
	gpu_results.sum += percent;
	if (percent < gpu_results.lowest) {
		gpu_results.lowest = percent;
		gpu_results.lowest_frame = frame;
	}
	if (percent < HLE_LOW_MATCH) {
		gpu_results.low++;
		printf("HLE: frame %d, %.2f%% of %d pixels\n", frame, percent, compared);
	}
}

/* -t: the whole composite with the HLE, its 3D filled by the SH2s' own rules on the CPU
   (sh2raster.c) instead of the GPU, must be exactly the core's frame: this checks
   everything around the 3D (the HUD and blanked lines kept from the frame buffer, the
   Mega Drive layer, the timing). Returns the number of rows that differ. */
static int check_hle_composite(int frame, uint16_t *ours, int report_first)
{
	static uint8_t image[HLE_IMAGE_WIDTH * HLE_IMAGE_HEIGHT];
	if (!hle_visible)
		return 0;
	memset(image, 0, sizeof(image));
	for (int i = 0; i < hle_visible->count; i++)
		sh2_draw_polygon(image, &hle_visible->polygons[i], hle_visible->view_top, hle_visible->view_bottom);
	use_texture_unit(UNIT_HLE_INDICES);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, HLE_IMAGE_WIDTH, HLE_IMAGE_HEIGHT, GL_RED_INTEGER, GL_UNSIGNED_BYTE, image);
	use_texture_unit(UNIT_CORE_FRAME);
	hle_colourize();

	int w = frame_width(), h = frame_height(), bad_rows = 0;
	composite(0, 0, w, h, hle_visible, 0);
	read_rgb565(w, h, ours);
	for (int y = 0; y < h; y++) {
		const uint16_t *core_row = &core_screen[(core_visible_top + y) * CORE_SCREEN_WIDTH];
		int x = 0;
		while (x < w && core_row[x] == ours[y * w + x])
			x++;
		if (x == w)
			continue;
		if (bad_rows == 0 && report_first)
			printf("HLE composite: first difference at frame %d line %d x %d\n", frame, y, x);
		bad_rows++;
	}
	return bad_rows;
}

/* -r: seams in the HLE's 3D (in the colour texture): pixels the 3D does not cover (less
   than half) with covered pixels on both sides, left and right or above and below. Most
   are gaps the scene really has (between the trees, say); a gap between polygons that
   should meet shows up as a jump in this count. */
static long count_seams(int w, int h, int view_top, int view_bottom)
{
	float *rgba = malloc((size_t)w * h * 4 * sizeof(float));
	GLint previous_framebuffer;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, hle_colour_framebuffer);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, rgba);
	glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);

	#define COVERED(x, y) (rgba[((size_t)(y) * w + (x)) * 4 + 3] >= 0.5f)
	long seams = 0;
	int top = (view_top + 1) * h / HLE_IMAGE_HEIGHT, bottom = view_bottom * h / HLE_IMAGE_HEIGHT;
	for (int y = top; y < bottom; y++)
		for (int x = 1; x < w - 1; x++)
			if (!COVERED(x, y) && ((COVERED(x - 1, y) && COVERED(x + 1, y)) || (COVERED(x, y - 1) && COVERED(x, y + 1))))
				seams++;
	#undef COVERED
	free(rgba);
	return seams;
}

/* -d: the composite with the HLE's 3D at `scale` times the console's resolution (with
   MSAA above 1x), as a w x h picture, written to path */
static void write_hle_picture(const char *path, int scale)
{
	int w = picture_width() * scale, h = frame_height() * scale;
	GLint previous_framebuffer;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
	double sky_offset;
	const hle_frame_t *hle = hle_to_draw(0, &sky_offset);
	if (hle)
		hle_draw_colour(hle, w, HLE_IMAGE_HEIGHT * scale, scale > 1);
	if (hle && scale > 1)
		printf("HLE seams at %dx (%s vertices): %ld pixels\n", scale, using_exact_vertices() ? "exact" : "original",
			count_seams(w, HLE_IMAGE_HEIGHT * scale, hle->view_top, hle->view_bottom));
	GLuint framebuffer = create_offscreen_framebuffer(w, h);
	composite(0, 0, w, h, hle, sky_offset);

	unsigned char *rgba = malloc((size_t)w * h * 4);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	FILE *file = fopen(path, "wb");
	if (file) {
		fprintf(file, "P6\n%d %d\n255\n", w, h);
		for (int y = h - 1; y >= 0; y--)          /* GL rows are bottom-up */
			for (int x = 0; x < w; x++)
				fwrite(rgba + ((size_t)y * w + x) * 4, 1, 3, file);
		fclose(file);
		printf("wrote %s (%dx%d)\n", path, w, h);
	}
	free(rgba);
	glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);
	glDeleteFramebuffers(1, &framebuffer);
}

/* -d: run frames 0..last, checking every frame; write the last one */
static int dump(int last, int hle_check_from, int picture_scale, int played_colours, const int *script_frame,
	const uint16_t *script_buttons, int script_length)
{
	static uint16_t ours[LAYERS_WIDTH * LAYERS_HEIGHT];
	int bad_frames = 0, bad_rows = 0;
	int hle_frames = 0, hle_bad_frames = 0;
	int plane_b_bad_frames = 0, plane_b_first_bad = -1;
	uint16_t buttons = 0;
	char path[40];

	create_offscreen_framebuffer(LAYERS_WIDTH, LAYERS_HEIGHT);
	for (int frame = 0; frame <= last; frame++) {
		for (int i = 0; i < script_length; i++) {
			if (script_frame[i] == frame)
				buttons = script_buttons[i];
		}
		const hle_frame_t *new_picture = run_frame(buttons);
		upload_frame();
		int frame_bad_rows = check_composite(frame, ours, bad_frames == 0);
		if (frame_bad_rows) {
			bad_frames++;
			bad_rows += frame_bad_rows;
		}
		if (hle_check_from >= 0 && frame >= hle_check_from) {
			if (hle_visible) {
				hle_frames++;
				if (check_hle_composite(frame, ours, hle_bad_frames == 0))
					hle_bad_frames++;
				if (check_plane_b(hle_visible) && plane_b_bad_frames++ == 0)
					plane_b_first_bad = frame;
			}
			if (new_picture)
				check_hle_gpu(frame, new_picture);
		}
	}
	printf("composite vs core: %d of %d frames differ (%d lines)\n", bad_frames, last + 1, bad_rows);
	if (hle_check_from >= 0)
		printf("HLE composite (SH2s' fill) vs core: %d of %d race frames differ\n", hle_bad_frames, hle_frames);
	if (hle_check_from >= 0)
		printf("plane B as we draw it vs PicoDrive's MD layer: %d of %d race frames differ (first %d)\n",
			plane_b_bad_frames, hle_frames, plane_b_first_bad);
	if (hle_check_from >= 0)
		printf("HLE (GPU) vs SH2s: %d pictures, average %.2f%%, lowest %.2f%% (frame %d), %d below %.0f%%\n",
			gpu_results.pictures, gpu_results.pictures ? gpu_results.sum / gpu_results.pictures : 100.0,
			gpu_results.lowest, gpu_results.lowest_frame, gpu_results.low, HLE_LOW_MATCH);

	snprintf(path, sizeof(path), "vrde_%06d.ppm", last);
	write_rgb565_ppm(path, frame_width(), frame_height(), &core_screen[core_visible_top * CORE_SCREEN_WIDTH],
		CORE_SCREEN_WIDTH);
	snprintf(path, sizeof(path), "composite_%06d.ppm", last);
	write_rgb565_ppm(path, frame_width(), frame_height(), ours, frame_width());
	if (hle_visible) {
		snprintf(path, sizeof(path), "hle_%06d.ppm", last);
		write_hle_picture(path, 1);
	}
	if (hle_visible || played_colours) {
		if (picture_scale > 1) {
			if (played_colours) {
				blend_dither = 1;
				picodrive_green = 0;
				smooth = SMOOTH_DISPLAY;
				widescreen = 1;
				set_side_reach();   /* as draw does: the far view's width */
				hle_set_near_limit(1);
				hle_set_far_view(far_view = FAR_VIEW_HAZE);
				hle_set_detailed_opponents(detailed_opponents = 1);
				upload_frame();
			}
			snprintf(path, sizeof(path), "hle_%06d_x%d.ppm", last, picture_scale);
			write_hle_picture(path, picture_scale);
		}
	}
	return bad_frames != 0;
}

/* ---- the window ---- */

static double now_seconds(void)
{
	return (double)SDL_GetPerformanceCounter() / SDL_GetPerformanceFrequency();
}

/* The real refresh rate, from the median interval of presenting a few blank frames.
   Events are handled in between: on macOS a window that has not handled any yet is not on
   screen, and a vsynced swap then waits for a refresh that never comes. */
static double measure_refresh(SDL_Window *window)
{
	double times[40], intervals[39];
	for (int i = 0; i < 40; i++) {
		SDL_PumpEvents();
		glClear(GL_COLOR_BUFFER_BIT);
		SDL_GL_SwapWindow(window);
		times[i] = now_seconds();
	}
	for (int i = 0; i < 39; i++)
		intervals[i] = times[i + 1] - times[i];
	for (int i = 0; i < 39; i++) {          /* sort, for the median */
		for (int j = i + 1; j < 39; j++) {
			if (intervals[j] < intervals[i]) {
				double swap = intervals[i];
				intervals[i] = intervals[j];
				intervals[j] = swap;
			}
		}
	}
	return 1.0 / intervals[19];
}

/* The window's title: the name, the version, the pictures drawn a second and the game's
   own pictures a second (the 3D pictures drawn in a race, otherwise the frames emulated),
   measured over each second */
static double title_fps, title_game_fps;   /* the last second's, for draw_debug_info too */

static void update_title(SDL_Window *window, double now)
{
	static double since = -1;
	static int pictures;
	if (since < 0)
		since = now;
	pictures++;
	if (now - since < 1.0)
		return;
	char title[96];
	int game_pictures = new_3d_pictures > 0 ? new_3d_pictures : new_emulated_frames;
	title_fps = pictures / (now - since);
	title_game_fps = game_pictures / (now - since);
	snprintf(title, sizeof(title), TITLE " - %.0f fps (Game %.0f)", title_fps, title_game_fps);
	SDL_SetWindowTitle(window, title);
	since = now;
	pictures = 0;
	new_3d_pictures = 0;
	new_emulated_frames = 0;
}

/* A window width that fits the picture (key 7) at this height */
static int window_width_for(int height)
{
	return (int)lround((double)height * picture_width() * PIXEL_ASPECT / 224);
}

/* Key 7: the window keeps its height and takes the picture's width (not in fullscreen,
   nor when the picture takes the window's shape) */
static void fit_window_width(SDL_Window *window)
{
	if ((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) || widescreen == ASPECT_AUTO)
		return;
	int window_w, window_h;
	SDL_GetWindowSize(window, &window_w, &window_h);
	SDL_SetWindowSize(window, window_width_for(window_h), window_h);
}

/* Under gamescope (SteamOS's game mode) the game is always fullscreen (main): SDL never
   sees it as fullscreen there (the flag stays off while the window takes the screen's
   size), so the menu shows it On and greyed instead of offering to switch */
static int under_gamescope;

/* Borderless fullscreen at the desktop's resolution and refresh rate (so the timing stays
   as it is), or back to the window. The mouse pointer is hidden while fullscreen. Returns
   0 if SDL could not switch: the window then stays as it was. */
static int set_fullscreen(SDL_Window *window, int on)
{
	if (SDL_SetWindowFullscreen(window, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0) {
		fprintf(stderr, "fullscreen %s: %s\n", on ? "on" : "off", SDL_GetError());
		return 0;
	}
	SDL_ShowCursor(on ? SDL_DISABLE : SDL_ENABLE);
	return 1;
}

/* Alt+Enter and the menu: switched by the player, so kept in vrde.ini for the next start */
static void toggle_fullscreen(SDL_Window *window)
{
	int on = !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
	if (set_fullscreen(window, on)) {
		fullscreen = on;
		save_settings();
	}
}

/* Each time the picture's size changes: what it is drawn at, the window, the desktop and
   whether SDL counts the window as fullscreen (under gamescope, SDL may never see the
   fullscreen it asked for while the size still changes) */
static void print_size_change(SDL_Window *window, int w, int h)
{
	static int last_w, last_h;
	if (w == last_w && h == last_h)
		return;
	last_w = w;
	last_h = h;
	int window_w, window_h;
	SDL_GetWindowSize(window, &window_w, &window_h);
	SDL_DisplayMode desktop = { 0 };
	SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(window), &desktop);
	fprintf(stderr, "picture %dx%d (window %dx%d, desktop %dx%d, fullscreen %s)\n", w, h, window_w, window_h,
		desktop.w, desktop.h, SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN ? "yes" : "no");
}

/* ---- audio ---- */

static struct {
	SDL_AudioDeviceID device;    /* 0 = no audio */
	int started;                 /* playback starts once the queue has reached its target */
	int underruns;               /* times the queue ran dry */
	long lowest_queue;           /* in samples, -1 = not measured yet */
	long highest_queue;          /* in samples */
	int dropped_frames;          /* frames of sound left out because the queue was too long */
	double position;             /* resampling: where the next output sample is, in input
	                                samples after the previous frame's last one */
	int16_t previous[2];         /* the previous frame's last stereo sample */
} audio = { 0, 0, 0, -1, 0, 0, 1.0, { 0, 0 } };

static void open_audio(void)
{
	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = SAMPLE_RATE;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 512;
	audio.device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (!audio.device) {
		fprintf(stderr, "no audio: %s\n", SDL_GetError());
		return;
	}
	char *name = NULL;
	SDL_AudioSpec device_spec;
	if (SDL_GetDefaultAudioInfo(&name, &device_spec, 0) != 0)
		name = NULL;
	for (char *c = name; c && *c; c++) {
		if ((unsigned char)*c >= 0x80)
			*c = '?';     /* a Windows console prints UTF-8 badly and may drop the rest */
	}
	printf("audio: %s, %d Hz, %d samples a buffer, %s\n", SDL_GetCurrentAudioDriver(),
		have.freq, have.samples, name ? name : "default device");
	SDL_free(name);
}

/* The frame's samples resampled by `stretch` (1.01 = 1% more samples), linearly, carrying
   the position over from frame to frame so the sound stays continuous. Returns the number
   of stereo samples written to out. */
static int resample(const int16_t *in, int count, double stretch, int16_t *out, int out_capacity)
{
	double step = 1.0 / stretch;
	int written = 0;
	/* sample i of "the previous frame's last sample, then this frame's" */
	#define SAMPLE(i, channel) ((i) == 0 ? audio.previous[channel] : in[((i) - 1) * 2 + (channel)])
	while (audio.position < count && written < out_capacity) {
		int i = (int)audio.position;
		double fraction = audio.position - i;
		for (int channel = 0; channel < 2; channel++) {
			double value = SAMPLE(i, channel) * (1.0 - fraction) + SAMPLE(i + 1, channel) * fraction;
			out[written * 2 + channel] = (int16_t)lround(value);
		}
		written++;
		audio.position += step;
	}
	#undef SAMPLE
	audio.position -= count;
	if (count > 0) {
		audio.previous[0] = in[(count - 1) * 2];
		audio.previous[1] = in[(count - 1) * 2 + 1];
	}
	return written;
}

/* Queue one frame's samples, stretched so that the queue stays near AUDIO_TARGET: a low
   queue gets a few more samples, a long one a few fewer */
static void queue_audio(void)
{
	static int16_t stretched[2 * (CORE_AUDIO_MAX_FRAMES + 64)];
	if (!audio.device)
		return;

	long queued = SDL_GetQueuedAudioSize(audio.device) / 4;     /* 4 bytes per stereo sample */
	double error = (double)(AUDIO_TARGET - queued) / AUDIO_TARGET;
	if (error > 1)
		error = 1;
	if (error < -1)
		error = -1;
	if (audio.started && queued == 0)
		audio.underruns++;
	if (audio.started && (audio.lowest_queue < 0 || queued < audio.lowest_queue))
		audio.lowest_queue = queued;
	if (queued > audio.highest_queue)
		audio.highest_queue = queued;
	/* far behind (the nudge alone would take minutes to catch up): leave this frame out */
	if (audio.started && queued > AUDIO_LIMIT) {
		audio.dropped_frames++;
		return;
	}

	int count = resample(core_audio_samples, core_audio_frame_count, 1.0 + AUDIO_NUDGE * error,
		stretched, CORE_AUDIO_MAX_FRAMES + 64);
	SDL_QueueAudio(audio.device, stretched, count * 4);
	if (!audio.started && queued + count >= AUDIO_TARGET) {
		SDL_PauseAudioDevice(audio.device, 0);
		audio.started = 1;
	}
}

/* ---- settings, the in-game menu and the keys ---- */

/* The settings keys 1 to 9 change, in the menu's order. The HLE (hle) and PicoDrive's
   colours (picodrive_green) are only in vrde.ini: the original view covers the first, and
   few would change the second. */
enum {
	SET_VIEW, SET_RESOLUTION, SET_MSAA, SET_VERTICES, SET_DITHER,
	SET_SMOOTH, SET_WIDESCREEN, SET_FAR_VIEW, SET_OPPONENTS, SET_COUNT
};

static const SDL_Scancode setting_keys[SET_COUNT] = {
	SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5,
	SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9
};
static const char setting_key_names[SET_COUNT + 1] = "123456789";

static const char *const setting_labels[SET_COUNT] = {
	"View", "Resolution", "MSAA", "Vertices", "Dither", "Framerate",
	"Aspect", "Far view", "Level of detail"
};

/* A setting's current value, as the menu shows it */
static const char *setting_value(int index)
{
	static char scale[8];
	switch (index) {
	case SET_VIEW:       return view == VIEW_COMPOSITE ? "Enhanced" : view == VIEW_ORIGINAL ? "Original" : "Side by side";
	case SET_RESOLUTION:
		if (resolution == RESOLUTION_WINDOW)
			return "Window";
		snprintf(scale, sizeof(scale), "%dx", resolution);
		return scale;
	case SET_MSAA:       return msaa ? "On" : "Off";
	case SET_VERTICES:   return using_exact_vertices() ? "Float" : "Integer";
	case SET_DITHER:     return blend_dither ? "Blended" : "Original";
	case SET_SMOOTH:     return smooth == SMOOTH_DISPLAY ? "Display" : smooth == SMOOTH_30 ? "30 fps"
	                          : smooth == SMOOTH_40 ? "40 fps" : smooth == SMOOTH_60 ? "60 fps" : "20 fps";
	case SET_WIDESCREEN: return widescreen == ASPECT_16_9 ? "16:9" : widescreen == ASPECT_21_9 ? "21:9"
	                          : widescreen == ASPECT_AUTO ? "Auto" : "4:3";
	case SET_FAR_VIEW:   return far_view == FAR_VIEW_HAZE ? "On + haze" : far_view ? "On" : "Off";
	default:             return detailed_opponents ? "Off" : "On";   /* level of detail: the game's simpler models further away */
	}
}

/* Whether a setting would change nothing now, so cannot be changed (the menu greys it
   out): the original view shows the core's own frame, which only the view affects; the
   SH2s' 3D has no resolution, MSAA, vertices, smooth motion, far view or opponents of
   ours; and smooth motion always draws exact vertices. */
static int setting_locked(int index)
{
	if (view == VIEW_ORIGINAL)
		return index != SET_VIEW;
	if (!hle_on)
		return index == SET_RESOLUTION || index == SET_MSAA || index == SET_VERTICES
			|| index == SET_SMOOTH || index == SET_FAR_VIEW || index == SET_OPPONENTS;
	return index == SET_VERTICES && smooth;
}

/* A setting one step forward (1) or back (-1); vrde.ini is rewritten */
static void change_setting(SDL_Window *window, int index, int step)
{
	static const int resolutions[] = { RESOLUTION_WINDOW, 1, 2, 4 };   /* key 2's order */
	if (setting_locked(index))
		return;
	switch (index) {
	case SET_VIEW:     view = (view + VIEW_COUNT + step) % VIEW_COUNT; break;
	case SET_RESOLUTION: {
		int i = 0;
		while (i < 3 && resolutions[i] != resolution)
			i++;
		resolution = resolutions[(i + 4 + step) % 4];
		break;
	}
	case SET_MSAA:     msaa = !msaa; break;
	case SET_VERTICES: exact_vertices = !exact_vertices; break;
	case SET_DITHER:   blend_dither = !blend_dither; break;
	case SET_SMOOTH: {
		static const int orders[] = { SMOOTH_OFF, SMOOTH_30, SMOOTH_40, SMOOTH_60, SMOOTH_DISPLAY };   /* key 6's */
		int i = 0;
		while (i < 4 && orders[i] != smooth)
			i++;
		smooth = orders[(i + 5 + step) % 5];
		break;
	}
	case SET_WIDESCREEN:   /* key 7's order: 16:9, 21:9, the window's, 4:3 */
		widescreen = (widescreen + 4 + step) % 4;
		fit_window_width(window);
		break;
	case SET_FAR_VIEW:
		far_view = (far_view + 3 + step) % 3;   /* off, on with haze, on */
		hle_set_far_view(far_view != FAR_VIEW_OFF);
		break;
	default:
		detailed_opponents = !detailed_opponents;
		hle_set_detailed_opponents(detailed_opponents);
		break;
	}
	save_settings();
}

/* The in-game menu, as in the MD version: Esc (Back or Guide on a controller) opens a
   panel over the paused game with the settings, the analog controls, fullscreen (a
   controller has no Alt+Enter: SteamOS's game mode), saving and loading the state, and
   quitting. The game's own fonts are compressed in the ROM, so the panel uses a small
   font of our own (menu_font). */
enum { MENU_ANALOG = SET_COUNT, MENU_FULLSCREEN, MENU_SAVE, MENU_LOAD, MENU_QUIT, MENU_ITEMS };

/* Colours, RGBA premultiplied by alpha */
static const uint8_t MENU_PANEL[4]   = { 8, 12, 30, 230 };
static const uint8_t MENU_CURSOR[4]  = { 30, 56, 150, 255 };
static const uint8_t MENU_OUTLINE[4] = { 0, 0, 0, 255 };
static const uint8_t MENU_TITLE[4]   = { 255, 214, 0, 255 };
static const uint8_t MENU_LABEL[4]   = { 235, 235, 235, 255 };
static const uint8_t MENU_VALUE[4]   = { 110, 200, 255, 255 };
static const uint8_t MENU_HINT[4]    = { 140, 140, 150, 255 };

static uint8_t menu_image[MENU_H][MENU_W][4];   /* the panel, top line first */

/* 5 x 7 glyphs, a byte a row from the top, bit 4 the leftmost pixel. Upper case only:
   the menu's text is shown in capitals. */
#define GLYPH_CROSS  '\1'
#define GLYPH_CIRCLE '\2'
static const struct {
	char c;
	uint8_t rows[7];
} menu_font[] = {
	{ 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
	{ 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
	{ 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
	{ 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
	{ 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
	{ 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
	{ 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
	{ 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
	{ 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
	{ 'J', { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C } },
	{ 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
	{ 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
	{ 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
	{ 'N', { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 } },
	{ 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
	{ 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
	{ 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D } },
	{ 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } },
	{ 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
	{ 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
	{ 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
	{ 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
	{ 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A } },
	{ 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
	{ 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } },
	{ 'Z', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F } },
	{ '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
	{ '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
	{ '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
	{ '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
	{ '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
	{ '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
	{ '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
	{ '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
	{ '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
	{ '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
	{ '[', { 0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E } },
	{ ']', { 0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E } },
	{ '(', { 0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02 } },
	{ ')', { 0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08 } },
	{ ':', { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 } },
	{ '/', { 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00 } },
	{ '+', { 0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00 } },
	{ '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
	{ '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } },
	{ '%', { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 } },
	{ GLYPH_CROSS,  { 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x00 } },   /* PlayStation buttons */
	{ GLYPH_CIRCLE, { 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E, 0x00 } },
};

static const uint8_t *glyph_rows(char c)
{
	for (size_t i = 0; i < SDL_arraysize(menu_font); i++) {
		if (menu_font[i].c == c)
			return menu_font[i].rows;
	}
	return NULL;   /* a space, or a character the font does not have: left blank */
}

static int glyph_pixel(const uint8_t *rows, int x, int y)
{
	return x >= 0 && x < 5 && y >= 0 && y < 7 && (rows[y] >> (4 - x) & 1);
}

/* A rectangle of the panel in one colour */
static void menu_fill(int x, int y, int w, int h, const uint8_t colour[4])
{
	for (int j = y; j < y + h; j++) {
		for (int i = x; i < x + w; i++)
			memcpy(menu_image[j][i], colour, 4);
	}
}

/* Text at panel pixel x, y (a line's top), in capitals with a black outline */
static void menu_text(int x, int y, const char *text, const uint8_t fill[4])
{
	for (; *text && x + CHAR_W <= MENU_W; text++, x += CHAR_W) {
		const uint8_t *rows = glyph_rows((char)toupper((unsigned char)*text));
		if (rows == NULL)
			continue;
		/* the glyph from (1, 2) in its cell; around it, one pixel of outline */
		for (int gy = -1; gy <= 7; gy++) {
			for (int gx = -1; gx <= 5; gx++) {
				int filled = glyph_pixel(rows, gx, gy), near_filled = 0;
				for (int dy = -1; dy <= 1; dy++) {
					for (int dx = -1; dx <= 1; dx++)
						near_filled |= glyph_pixel(rows, gx + dx, gy + dy);
				}
				if (filled || near_filled)
					memcpy(menu_image[y + 2 + gy][x + 1 + gx], filled ? fill : MENU_OUTLINE, 4);
			}
		}
	}
}

/* The panel's last line: the keys, and the buttons by the names on the pad in use (the
   bottom one closes, the right one changes, whatever they are called) */
static const char *menu_hint(void)
{
	if (!controller)
		return "Esc: close   Enter: change";
	switch (SDL_GameControllerGetType(controller)) {
	case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
	case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
	case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
	case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
		return "Esc/B: close   Enter/A: change";
	case SDL_CONTROLLER_TYPE_PS3:
	case SDL_CONTROLLER_TYPE_PS4:
	case SDL_CONTROLLER_TYPE_PS5:
		return "Esc/\1: close   Enter/\2: change";
	default:
		return "Esc/A: close   Enter/B: change";
	}
}

/* The panel: a title, the settings, the analog controls, fullscreen, the state and
   quitting, and how to use it */
static void build_menu_image(void)
{
	const char *hint = menu_hint();
	menu_fill(0, 0, MENU_W, MENU_H, MENU_PANEL);
	menu_text((MENU_W - CHAR_W * 8) / 2, MENU_TOP, "SETTINGS", MENU_TITLE);

	for (int item = 0; item < MENU_ITEMS; item++) {
		int line = item < SET_COUNT ? 2 + item : 3 + item;   /* the gaps: see MENU_LINES */
		int y = MENU_TOP + CHAR_H * line;
		const char *value = NULL;
		char label[MENU_COLUMNS + 1];
		if (item < SET_COUNT) {
			snprintf(label, sizeof(label), "[%c] %s", setting_key_names[item], setting_labels[item]);
			value = setting_value(item);
		} else if (item == MENU_ANALOG) {
			strcpy(label, "    Analog control");
			value = analog ? "On" : "Off";
		} else if (item == MENU_FULLSCREEN) {
			strcpy(label, "    Fullscreen");   /* Alt+Enter: no room for it here */
			value = under_gamescope || SDL_GetWindowFlags(SDL_GL_GetCurrentWindow()) & SDL_WINDOW_FULLSCREEN ? "On" : "Off";
		} else if (item == MENU_SAVE) {
			strcpy(label, "[O] Save state");
		} else if (item == MENU_LOAD) {
			strcpy(label, "[P] Load state");
		} else {
			strcpy(label, "    Quit");
		}
		if (item == menu.selected && menu.status)
			value = menu.status;

		if (item == menu.selected)
			menu_fill(4, y, MENU_W - 8, CHAR_H, MENU_CURSOR);
		menu_text(8, y, label, MENU_LABEL);
		if (value) {
			int locked = (item < SET_COUNT && setting_locked(item)) || (item == MENU_FULLSCREEN && under_gamescope);
			menu_text(8 + CHAR_W * (MENU_COLUMNS - (int)strlen(value)), y, value, locked ? MENU_HINT : MENU_VALUE);
		}
	}
	menu_text((MENU_W - CHAR_W * (int)strlen(hint)) / 2, MENU_TOP + CHAR_H * (MENU_LINES - 1), hint, MENU_HINT);
}

/* The open menu over a w x h window, in the middle, at the largest whole scale that fits */
static void draw_menu(int w, int h)
{
	int scale = w / MENU_W < h / MENU_H ? w / MENU_W : h / MENU_H;
	if (scale < 1)
		scale = 1;
	build_menu_image();
	use_texture_unit(UNIT_MENU);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, MENU_W);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, MENU_W, MENU_H, GL_RGBA, GL_UNSIGNED_BYTE, menu_image);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	use_texture_unit(UNIT_CORE_FRAME);

	glViewport((w - MENU_W * scale) / 2, (h - MENU_H * scale) / 2, MENU_W * scale, MENU_H * scale);
	glUseProgram(menu_program);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisable(GL_BLEND);
	glUseProgram(blit_program);
}

/* Lines of text at the window's top left from line `first` down, in the menu's letters at
   the menu's scale */
static void draw_text_lines(int w, int h, int first, const char *const *lines, int count)
{
	int scale = w / MENU_W < h / MENU_H ? w / MENU_W : h / MENU_H;
	if (scale < 1)
		scale = 1;
	int text_w = 0, text_h = CHAR_H * count;
	for (int i = 0; i < count; i++) {
		int line_w = CHAR_W * (int)strlen(lines[i]);
		text_w = line_w > text_w ? line_w : text_w;
	}
	text_w = text_w < MENU_W ? text_w : MENU_W;
	static const uint8_t clear[4] = { 0, 0, 0, 0 };
	menu_fill(0, 0, text_w, text_h, clear);
	for (int i = 0; i < count; i++)
		menu_text(0, CHAR_H * i, lines[i], MENU_TITLE);
	use_texture_unit(UNIT_MENU);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, MENU_W);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, text_w, text_h, GL_RGBA, GL_UNSIGNED_BYTE, menu_image);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	use_texture_unit(UNIT_CORE_FRAME);

	int margin = 8 * scale;
	glViewport(margin, h - margin - (CHAR_H * first + text_h) * scale, text_w * scale, text_h * scale);
	glUseProgram(menu_program);
	set_vec2(menu_program, "uv_scale", (float)text_w / MENU_W, (float)text_h / MENU_H);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisable(GL_BLEND);
	set_vec2(menu_program, "uv_scale", 1, 1);
	glUseProgram(blit_program);
}

#define DEBUG_LINES 4

/* Debug info: the frame rates as in the title (which a fullscreen game hides), the race's
   3D resolution (none outside a race), and the analog controls */
static void draw_debug_info(int w, int h)
{
	if (!debug_info)
		return;
	char lines[DEBUG_LINES][MENU_COLUMNS + 1];
	snprintf(lines[0], sizeof(lines[0]), "%.0f fps  game %.0f", title_fps, title_game_fps);
	if (drawn_3d_w)
		snprintf(lines[1], sizeof(lines[1]), "resolution %dx%d%s", drawn_3d_w, drawn_3d_h, msaa ? " msaa" : "");
	else
		snprintf(lines[1], sizeof(lines[1]), "resolution none");
	/* the stick (percent of its way, left negative), the target it gives (left positive, 0 =
	   the D-pad steers) and the car's steering in the same units ($908E / 256); the triggers'
	   amounts (0 = the buttons) and the car's pedals ($900E, $9010) */
	const uint16_t *ram = (const uint16_t *)PicoMem.ram;
	snprintf(lines[2], sizeof(lines[2]), "stick %d%% target %d car %d", analog_input.stick_axis * 100 / 32767,
		analog_input.steering, (short)ram[0x908E >> 1] / 256);
	snprintf(lines[3], sizeof(lines[3]), "throttle %d/%d brake %d/%d", analog_input.throttle,
		(short)ram[0x900E >> 1], analog_input.brake, (short)ram[0x9010 >> 1]);
	const char *text[DEBUG_LINES] = { lines[0], lines[1], lines[2], lines[3] };
	draw_text_lines(w, h, 0, text, DEBUG_LINES);
}

/* The notice (show_notice), while it lasts and the menu is closed (the menu shows its
   own): below the debug info when that is on */
static void draw_notice(int w, int h)
{
	if (notice.text == NULL || menu.open || now_seconds() >= notice.until)
		return;
	draw_text_lines(w, h, debug_info ? DEBUG_LINES : 0, &notice.text, 1);
}

/* Opening the menu reads vrde.ini again and applies it, so that it can be edited while the
   game runs (the settings only there too: the far view's distance and window, the haze) */
static void reload_settings(SDL_Window *window)
{
	int was_widescreen = widescreen;
	settings_load(SETTINGS_FILE, settings, SETTING_COUNT);
	apply_hle_settings();
	if (widescreen != was_widescreen)
		fit_window_width(window);
}

/* The game pauses while the menu is open, its sound too */
static void set_menu_open(SDL_Window *window, int open)
{
	if (open)
		reload_settings(window);
	menu.open = open;
	menu.status = NULL;
	if (!open)
		menu.wait_for_release = 1;
	if (audio.device && audio.started)
		SDL_PauseAudioDevice(audio.device, open);
}

static void move_cursor(int step)
{
	menu.selected = (menu.selected + MENU_ITEMS + step) % MENU_ITEMS;
	menu.status = NULL;
}

/* Left or right on the selected item: a setting one step back or forward; the analog
   controls and fullscreen on or off */
static void change_item(SDL_Window *window, int step)
{
	if (menu.selected < SET_COUNT) {
		change_setting(window, menu.selected, step);
	} else if (menu.selected == MENU_ANALOG) {
		analog = !analog;   /* the dead zones are in vrde.ini only */
		save_settings();
	} else if (menu.selected == MENU_FULLSCREEN && !under_gamescope) {
		toggle_fullscreen(window);
	}
}

/* Enter or A on the selected item; returns 0 to quit */
static int confirm_item(SDL_Window *window)
{
	if (menu.selected < MENU_SAVE)
		change_item(window, 1);
	else if (menu.selected == MENU_SAVE)
		menu.status = save_state() ? "Saved" : "Failed";
	else if (menu.selected == MENU_LOAD && load_state(STATE_FILE))
		set_menu_open(window, 0);
	else if (menu.selected == MENU_LOAD)
		menu.status = "No save";
	else
		return 0;
	return 1;
}

/* A key: Esc opens or closes the menu, which takes the arrows and Enter while open; 1 to 9
   change the settings, O and P save and load the state, Alt+Enter toggles fullscreen. Keys
   are taken by their place on the keyboard (scancode), as the game's are (read_pad): the
   character a key types depends on the layout, and under Wine on Linux it can come out
   wrong altogether. Returns 0 to quit. */
static int handle_key(SDL_Window *window, SDL_Scancode key)
{
	int alt_held = (SDL_GetModState() & KMOD_ALT) != 0;
	if ((key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER) && alt_held) {
		if (!under_gamescope)
			toggle_fullscreen(window);
		return 1;
	}
	if (key == SDL_SCANCODE_ESCAPE) {
		set_menu_open(window, !menu.open);
		return 1;
	}
	if (menu.open) {
		switch (key) {
		case SDL_SCANCODE_UP:
			move_cursor(-1);
			return 1;
		case SDL_SCANCODE_DOWN:
			move_cursor(1);
			return 1;
		case SDL_SCANCODE_LEFT:
		case SDL_SCANCODE_RIGHT:
			change_item(window, key == SDL_SCANCODE_LEFT ? -1 : 1);
			return 1;
		case SDL_SCANCODE_RETURN:
		case SDL_SCANCODE_KP_ENTER:
		case SDL_SCANCODE_SPACE:
			return confirm_item(window);
		default:
			break;
		}
	}
	for (int i = 0; i < SET_COUNT; i++) {
		if (key == setting_keys[i])
			change_setting(window, i, 1);
	}
	if (key == SDL_SCANCODE_O)
		save_state();
	else if (key == SDL_SCANCODE_P)
		load_state(STATE_FILE);
	return 1;
}

/* A controller button: Back or Guide opens or closes the menu; while it is open the D-pad
   moves and changes, the right face button (SDL's B) confirms and the bottom one (A)
   closes, by position, so they do the same on every pad (see the controller map).
   Returns 0 to quit. */
/* The debug info on or off: as the second of the two sticks goes down while the other is
   held, in either order; pressing again (one released and down again) turns it back */
static void toggle_debug_info(void)
{
	if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSTICK)
			&& SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSTICK))
		debug_info = !debug_info;
}

static int handle_controller_button(SDL_Window *window, int button)
{
	if (button == SDL_CONTROLLER_BUTTON_LEFTSTICK || button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) {
		toggle_debug_info();
		return 1;
	}
	if (button == SDL_CONTROLLER_BUTTON_BACK || button == SDL_CONTROLLER_BUTTON_GUIDE) {
		set_menu_open(window, !menu.open);
		return 1;
	}
	if (!menu.open)
		return 1;
	switch (button) {
	case SDL_CONTROLLER_BUTTON_DPAD_UP:
		move_cursor(-1);
		break;
	case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
		move_cursor(1);
		break;
	case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
	case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
		change_item(window, button == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? -1 : 1);
		break;
	case SDL_CONTROLLER_BUTTON_B:
		return confirm_item(window);
	case SDL_CONTROLLER_BUTTON_A:
		set_menu_open(window, 0);
		break;
	}
	return 1;
}

/* Returns 0 when the program should end */
static int handle_events(SDL_Window *window)
{
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_QUIT)
			return 0;
		if (event.type == SDL_KEYDOWN && !event.key.repeat && !handle_key(window, event.key.keysym.scancode))
			return 0;
		if (event.type == SDL_CONTROLLERBUTTONDOWN && !handle_controller_button(window, event.cbutton.button))
			return 0;
	}
	return 1;
}

/* ---- main ---- */

static void print_usage(const char *program)
{
	fprintf(stderr, "usage: %s [-v] [-d N [-t F] [-r R] [-f] [-b]] [-l state] [-p frame:keys]... [-o file] rom.32x\n", program);
}

/* The OpenGL 3.3 core window (hidden for -d) */
static SDL_Window *open_window(int hidden)
{
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
	int width = window_width_for(224 * 3);
	SDL_Window *window = SDL_CreateWindow(TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		width, 224 * 3, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
		(hidden ? SDL_WINDOW_HIDDEN : 0));
	if (!window || !SDL_GL_CreateContext(window)) {
		fprintf(stderr, "window/GL context: %s\n", SDL_GetError());
		return NULL;
	}
	if (!gl_load_functions())
		return NULL;
	/* SDL turns text input on by default; on macOS a held letter key would open the
	   accent / input method popup */
	SDL_StopTextInput();
	gl_init();
	return window;
}

int main(int argc, char **argv)
{
	int dump_frame = -1;
	int hle_check_from = -1;
	int picture_scale = 1;
	int exact_check = 0;
	int played_colours = 0;
	const char *state_path = NULL;
	int script_frame[MAX_SCRIPT];
	uint16_t script_buttons[MAX_SCRIPT];
	int script_length = 0;

	/* print each line at once, also when the output goes to a file */
	setvbuf(stdout, NULL, _IOLBF, 0);
#ifdef _WIN32
	setvbuf(stdout, NULL, _IONBF, 0);          /* Windows has no line buffering: unbuffered */
	SetUnhandledExceptionFilter(report_crash);
#endif

	int option;
	const char *record_path = NULL;
	while ((option = getopt(argc, argv, "bd:fl:o:p:r:t:v")) != -1) {
		char letters[16] = "";
		if (option == 'v') {
			puts(TITLE);
			return 0;
		} else if (option == 'd') {
			dump_frame = atoi(optarg);
		} else if (option == 'l') {
			state_path = optarg;
		} else if (option == 'o') {
			record_path = optarg;
		} else if (option == 't') {
			hle_check_from = atoi(optarg);
		} else if (option == 'r') {
			picture_scale = atoi(optarg);
		} else if (option == 'f') {
			exact_check = 1;
		} else if (option == 'b') {
			played_colours = 1;
		} else if (option == 'p' && script_length < MAX_SCRIPT
			&& sscanf(optarg, "%d:%15s", &script_frame[script_length], letters) >= 1) {
			script_buttons[script_length] = core_parse_buttons(letters);
			script_length++;
		} else {
			print_usage(argv[0]);
			return 1;
		}
	}
	if (optind != argc - 1) {
		print_usage(argv[0]);
		return 1;
	}
	const char *rom = argv[optind];
	int checking = dump_frame >= 0;
	if (checking) {
		resolution = 1;      /* the checks compare at the console's resolution */
		msaa = 0;
		exact_vertices = exact_check;
		smooth = 0;
		widescreen = 0;
		picodrive_green = 1;  /* the core's colours, bit for bit */
		blend_dither = 0;
		layers_read_past_buffer_end = 1;
		far_view = FAR_VIEW_OFF;
		detailed_opponents = 0;
	} else {
		load_settings();
	}
	apply_hle_settings();
	hle_set_near_limit(!checking);   /* the checks compare with the SH2s' picture */

	SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");   /* face buttons by position */
	/* Windows: sizes in scaled points and the drawable in real pixels, as on macOS,
	   instead of the system enlarging (blurring) the window on a high-DPI display */
	SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
	SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
	Uint32 subsystems = SDL_INIT_VIDEO | (checking ? 0 : SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER);
	if (SDL_Init(subsystems) < 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Window *window = open_window(checking);
	if (!window)
		return 1;
	/* Under gamescope (SteamOS's game mode) the window can't be resized: gamescope only
	   scales it up to the screen. Fullscreen takes gamescope's resolution instead. */
	under_gamescope = !checking && getenv("GAMESCOPE_WAYLAND_DISPLAY") != NULL;
	if (under_gamescope)
		set_fullscreen(window, 1);

	if (core_init(rom, SAMPLE_RATE) != 0)
		return 1;
	/* the analog controls in their dead zones leave the game as it is; the checks run without
	   them */
	if (!checking)
		analog_install();
	layers_init();
	vrde_comm_write_hook = on_comm_write;
	vrde_dreq_write_hook = on_dreq_write;
	vrde_sh2_access_hook = on_sh2_access;
	/* The core finishes starting up in its first frame: a state loaded before that crashed
	   it in the first frame after (in the PSG's update). Loading with key P, frames have
	   run already. */
	if (state_path)
		core_run_frame(0);
	if (state_path && !load_state(state_path)) {
		fprintf(stderr, "%s: not a saved state\n", state_path);
		return 1;
	}
	if (record_path && !checking) {
		input_record = fopen(record_path, "w");
		if (!input_record) {
			perror(record_path);
			return 1;
		}
	}

	if (checking) {
		printf("checking frames 0 to %d\n", dump_frame);
		return dump(dump_frame, hle_check_from, picture_scale, played_colours, script_frame, script_buttons, script_length);
	}

	/* Pace: one emulated frame per vsync near 60 Hz, every second one near 120 Hz,
	   otherwise by the clock */
	SDL_GL_SetSwapInterval(1);
	double hz = measure_refresh(window), fps;
	/* Fullscreen as the player left it only now: on macOS it moves the window to a Space of
	   its own with an animation, which the measurement must not see. Borderless, it keeps
	   the desktop's refresh rate. */
	if (fullscreen && !under_gamescope)
		set_fullscreen(window, 1);
	int frames_per_vsync;
	if (fabs(hz - NTSC_FPS) < NTSC_FPS * VSYNC_LOCK) {
		frames_per_vsync = 1;
		fps = hz;
	} else if (fabs(hz / 2 - NTSC_FPS) < NTSC_FPS * VSYNC_LOCK) {
		frames_per_vsync = 2;
		fps = hz / 2;
	} else {
		frames_per_vsync = 0;      /* time-based */
		fps = NTSC_FPS;
	}
	printf("OpenGL: %s (%s)\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
	printf("display %.2f Hz: %s, emulating at %.3f fps\n", hz,
		frames_per_vsync == 1 ? "1 frame per vsync" : frames_per_vsync == 2 ? "1 frame every 2 vsyncs" : "time-based",
		fps);
	open_audio();

	double refresh_period = 1.0 / hz;  /* as measured at start (a median: steady under jitter) */
	double last_interval = 1.0 / hz;   /* real time between the last two refreshes */
	double last_time = now_seconds(), time_accumulator = 0;
	double interval_sum = 0, interval_sum_squares = 0, worst_interval = 0;
	long vsyncs = 0, frames_emulated = 0;
	long periods = 0;                  /* refresh periods passed (a missed vsync counts all of them) */
	double vsync_anchor = last_time;   /* a moment of a vsync: periods are counted from it in
	                                      whole refresh periods, so a late swap and the early
	                                      one after it add up to two, not three */
	long next_frame_period = 0;        /* the period the next frame is due at */
	int missed_vsyncs = 0;
	/* where each refresh's time goes, and the missed vsyncs by frames emulated (0, 1, 2+) */
	double emulate_sum = 0, draw_sum = 0, swap_sum = 0;
	double emulate_worst = 0, draw_worst = 0, swap_worst = 0;
	int missed_by_frames[3] = { 0, 0, 0 };
	int running = 1;
	while (running) {
		running = handle_events(window);
		update_title(window, now_seconds());

		/* how many frames to emulate before this refresh */
		int frames_to_run = 0;
		if (menu.open) {
			/* paused: nothing comes due, so nothing is caught up after */
			next_frame_period = periods;
			time_accumulator = 0;
		} else if (frames_per_vsync) {
			/* a frame every frames_per_vsync refresh periods; after a missed vsync the
			   frames due meanwhile are caught up (up to 4), so the game keeps time and the
			   sound stays fed */
			while (periods >= next_frame_period && frames_to_run < 4) {
				next_frame_period += frames_per_vsync;
				frames_to_run++;
			}
			if (periods >= next_frame_period)
				next_frame_period = periods + frames_per_vsync;   /* too far behind: skip */
		} else {
			/* the real time since the last refresh, not one refresh period: a window covered
			   by others gets its refreshes late and irregularly (macOS), and the game must
			   keep time */
			time_accumulator += last_interval;
			while (time_accumulator >= 1.0 / fps && frames_to_run < 4) {
				time_accumulator -= 1.0 / fps;
				frames_to_run++;
			}
			if (time_accumulator >= 1.0 / fps)
				time_accumulator = 0;                             /* too far behind: skip */
		}

		double emulate_start = now_seconds();
		for (int i = 0; i < frames_to_run; i++) {
			uint16_t buttons = read_pad();
			read_analog();
			record_input(buttons, analog_input.steering, analog_input.throttle, analog_input.brake);
			analog_set_input(analog_input.steering, analog_input.throttle, analog_input.brake);
			run_frame(buttons);
			frames_emulated++;
			new_emulated_frames++;
			queue_audio();
		}
		if (frames_to_run)
			upload_frame();

		double draw_start = now_seconds();
		int w, h;
		SDL_GL_GetDrawableSize(window, &w, &h);
		print_size_change(window, w, h);
		/* paused, the picture stays where it was when the menu opened */
		static double fraction;
		if (!menu.open)
			fraction = frame_fraction(frames_per_vsync, periods, next_frame_period, time_accumulator * fps);
		draw(w, h, fraction);
		if (menu.open)
			draw_menu(w, h);
		draw_debug_info(w, h);
		draw_notice(w, h);
		double swap_start = now_seconds();
		SDL_GL_SwapWindow(window);

		/* timing: a running average of the vsync interval (the emulated frame rate follows
		   it), missed vsyncs, and statistics for the end, all after the first 60 vsyncs */
		double current_time = now_seconds();
		double interval = current_time - last_time;
		last_time = current_time;
		last_interval = interval;
		int missed = interval >= 1.5 * refresh_period;
		double position = (current_time - vsync_anchor) / refresh_period;
		long nearest = lround(position);
		/* follow the vsyncs' phase slowly: a refresh period measured a little off only
		   leaves the swaps a steady fraction of a period from the count */
		vsync_anchor += (position - nearest) * refresh_period * 0.01;
		if (nearest > periods)
			periods = nearest;
		if (vsyncs > 60) {
			double emulate_time = draw_start - emulate_start;
			double draw_time = swap_start - draw_start;
			double swap_time = current_time - swap_start;
			emulate_sum += emulate_time;
			draw_sum += draw_time;
			swap_sum += swap_time;
			emulate_worst = fmax(emulate_worst, emulate_time);
			draw_worst = fmax(draw_worst, draw_time);
			swap_worst = fmax(swap_worst, swap_time);
			if (missed) {
				missed_vsyncs++;
				missed_by_frames[frames_to_run < 2 ? frames_to_run : 2]++;
			}
			interval_sum += interval;
			interval_sum_squares += interval * interval;
			if (interval > worst_interval)
				worst_interval = interval;
		}
		vsyncs++;
	}

	if (vsyncs > 61) {
		long n = vsyncs - 61;
		double mean = interval_sum / n;
		double deviation = sqrt(fmax(0, interval_sum_squares / n - mean * mean));
		printf("%ld vsyncs, %ld frames emulated; vsync interval %.3f ms (sd %.3f, worst %.3f), %d missed vsyncs\n",
			vsyncs, frames_emulated, mean * 1000, deviation * 1000, worst_interval * 1000, missed_vsyncs);
		printf("per vsync: emulate %.3f ms (worst %.3f), draw %.3f ms (worst %.3f), swap %.3f ms (worst %.3f); "
			"missed after 0/1/2+ frames emulated: %d/%d/%d\n",
			emulate_sum / n * 1000, emulate_worst * 1000, draw_sum / n * 1000, draw_worst * 1000,
			swap_sum / n * 1000, swap_worst * 1000, missed_by_frames[0], missed_by_frames[1], missed_by_frames[2]);
		printf("audio: %d underruns, queue %.1f to %.1f ms (target %.1f ms), %d frames dropped; final frame rate %.3f fps\n",
			audio.underruns, audio.lowest_queue * 1000.0 / SAMPLE_RATE, audio.highest_queue * 1000.0 / SAMPLE_RATE,
			AUDIO_TARGET * 1000.0 / SAMPLE_RATE, audio.dropped_frames, fps);
	}
	SDL_Quit();
	return 0;
}
