/* The race's 3D picture, computed from the scene the 68000 sends to the SH2s each tick,
   the same way the SH2s compute it (high-level emulation: HLE).

   Each tick the 68000 copies a 2560-byte scene to SH2 SDRAM 0600C000 (camera, objects,
   track segments; see docs/scene-data*). The SH2s turn it into a list of screen
   polygons and fill them into the 32X frame buffer. hle_build() makes the same list,
   with the same integer arithmetic, so that it can be checked pixel by pixel against
   the SH2s' picture; the front end then draws it with the GPU. */
#ifndef VRDE_HLE_H
#define VRDE_HLE_H

#include <stdint.h>

#define HLE_SCENE_ADDRESS 0x0600C000   /* in SH2 SDRAM */
#define HLE_SCENE_SIZE    0xA00

#define HLE_MAX_POLYGONS 8192

typedef struct {
	/* The colour word of the SH2's polygon record: palette index in the high byte,
	   bit 0 = triangle, bit 1 = dithered (only every other pixel is drawn). */
	uint16_t colour;
	int vertex_count;          /* 3 or 4 */
	/* Screen position of each vertex, relative to the screen centre (160, 112),
	   y pointing up, as in the SH2's records. */
	int16_t x[4], y[4];
	/* The same, computed exactly (no rounding anywhere): for drawing at a high
	   resolution. The SH2's x, y are these rounded down, give or take its rounding on
	   the way. */
	float fx[4], fy[4];
	/* Each vertex's distance in front of the camera, exactly, in world units (those of x
	   and z); 0 for the HUD's polygons. For the far view's haze. */
	float depth[4];
	/* Which vertices draw it (HLE_BOTH_VERTICES and so on). The SH2s test which way a
	   polygon faces on their rounded screen positions; that drops thin polygons that
	   face the camera, and on rare occasions keeps one that does not. */
	int drawn_with;
	/* For the HUD's polygons, where their model is placed on screen: its x, relative to the
	   centre as x is (the course map's +100, the needle's -148; 0 for a model with no
	   place of its own and for the 3D) */
	int hud_anchor;
} hle_polygon_t;

enum {
	HLE_BOTH_VERTICES,         /* faces the camera either way */
	HLE_SH2_VERTICES_ONLY,     /* the SH2s drew it, but it faces away exactly */
	HLE_EXACT_VERTICES_ONLY,   /* faces the camera exactly, but the SH2s dropped it */
};

typedef struct {
	int view_top, view_bottom;   /* the lines the 3D is clipped to (16 and 207 in a race) */
	double camera_t;             /* how far the camera is from the older scene to the newer
	                                (hle_interpolated): 0 to 1, and 1 across a camera cut */
	int command;                 /* the command it was drawn for (HLE_RACE_COMMAND, ...) */
	int scene;                   /* hle_scene_count() when it was built: its newer scene */
	int hud_first;               /* polygons from here on are the HUD's (needle, map) */
	int count;
	hle_polygon_t polygons[HLE_MAX_POLYGONS];   /* in drawing order: far to near, then the HUD's */
} hle_frame_t;

/* The 68000's commands that draw a race picture from a scene */
#define HLE_RACE_COMMAND 2   /* a race (and its result screens) */
#define HLE_DEMO_COMMAND 5   /* the race demo before the title */

/* Builds the polygon list from a scene (HLE_SCENE_SIZE bytes, big-endian, as the
   68000 sent it) for one of the commands above. Models are read from the ROM and SH2
   SDRAM through the core. Returns 0 when the command draws no 3D this time (the demo
   can leave it out). */
int hle_build(const uint8_t *scene, int command, hle_frame_t *frame);

/* The scene currently in SH2 SDRAM, as big-endian bytes. */
void hle_read_scene(uint8_t *scene);

/* Following the game, so that the polygon list matches the frame buffer on screen.
   Call these from the core's COMM and DREQ hooks (see pico.h); they build a list each
   time a race scene arrives. */
void hle_on_comm_write(int writer, unsigned int offset, unsigned int value, int size);
void hle_on_dreq_write(unsigned int address);

/* Call after each frame. When the 32X has just switched to a newly finished frame
   buffer, returns the polygon list of that picture (NULL if it is not a race picture);
   otherwise NULL. */
const hle_frame_t *hle_on_frame_end(void);

/* The polygon list of the picture on screen, or NULL. */
const hle_frame_t *hle_shown(void);

/* The polygon list of the last scene sent, or NULL */
const hle_frame_t *hle_latest(void);

/* Smooth motion: the polygon list between the last two scenes (the game's ticks),
   `fraction` of a frame after the last hle_on_frame_end; NULL before the first scene.
   It reaches each scene when the 32X starts showing the SH2s' picture of it. Built anew
   at each call, with exact vertices (fx, fy) meant for drawing. Its 3D lines are those of
   on_screen (if not NULL), the picture whose frame buffer is on screen: what lies around
   the 3D (a closing transition) is that picture's. frames_per_image (if not 0) holds each
   image for so many emulated frames, on a grid from frame 0: 1 makes 60 images a second,
   1.5 makes 40 (two a tick), 2 makes 30. */
const hle_frame_t *hle_interpolated(double fraction, const hle_frame_t *on_screen, double frames_per_image);

/* Far view: also draws the track the 68000 does not send, and what the SH2s leave out
   for being too far away or too far to the side, up to twice as far (z 512, about
   8200 world units). Off (the default), the list is the SH2s'. */
void hle_set_far_view(int on);

/* How far the far view reaches, in world units: 8192 by default, from FAR_VIEW_NEAREST
   (the SH2s' own limit) to FAR_VIEW_FURTHEST. */
#define FAR_VIEW_NEAREST  4080
#define FAR_VIEW_FURTHEST 16384
void hle_set_far_view_distance(int world_units);

/* The tiles of the course's grid (cells, 1024 world units square) the far view adds track
   and props in: those reached in so many steps from the player's car's cell along cells
   with a model (8 by default; the 68000 sends its own 3 ahead, 1 behind and 2 to each side). */
void hle_set_far_view_tiles(int tiles);

/* Detailed opponents: every opponent drawn with the detailed model and wheels the game
   gives only the near ones. Off (the default), the 68000's models. */
void hle_set_detailed_opponents(int on);

/* How far to the side the far view keeps the track and what the SH2s leave out, in 256ths
   of the depth: at least HLE_SIDE_REACH_16_9, twice the depth (the 16:9 picture's edges
   are at 1.95 times it); more for a wider picture. */
#define HLE_SIDE_REACH_16_9 512
void hle_set_side_reach(int reach);

/* On, the track's and the roadside's near plane is at most 0x4000, so that the ground right
   beside the car, which our wider picture shows, is not cut away; off (the default), the
   game's, as the SH2s draw */
void hle_set_near_limit(int on);

/* How many scenes have been sent so far (counts up by one with each) */
int hle_scene_count(void);

/* Forgets the scenes seen so far, after loading a saved state: until the next picture
   is finished the SH2s' picture is shown. */
void hle_reset(void);

#endif
