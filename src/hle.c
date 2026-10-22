/* The race's 3D picture from the scene (see hle.h).

   This follows the SH2 code of the race command (master SH2 06000CFC) step by step:
     1. object matrices (master, 06000DC8)
     2. sorting the track segment list (master, 0600115C)
     3. geometry: transform, project, cull, clip at the near plane, light, and put each
        polygon in a depth bucket (code overlay A, run by both SH2s on different objects)
     4. the HUD's polygons, such as the rev counter needle and the course map (master,
        060010AC); they skip the depth buckets and are drawn last
     5. drawing order: the depth buckets from far to near (code overlay B, 060011B4)
   The arithmetic is the SH2's: 16.16 fixed point, MAC.L sums, the SH2's divider.
   Addresses in comments are SH2 addresses (overlay code runs from the SH2's cache at
   C0000000). Details and how they were found: docs/scene-data*. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "pico/pico_int.h"
#include "hle.h"

/* ---- The SH2's arithmetic ---- */

/* MAC.L over three pairs, then XTRCT: bits 16-47 of the 64-bit sum (16.16 result) */
static int32_t dot3(const int32_t *a, const int32_t *b)
{
	int64_t sum = (int64_t)a[0] * b[0] + (int64_t)a[1] * b[1] + (int64_t)a[2] * b[2];
	return (int32_t)(sum >> 16);
}

/* DMULS.L then XTRCT: a 16.16 product */
static int32_t multiply_16(int32_t a, int32_t b)
{
	return (int32_t)(((int64_t)a * b) >> 16);
}

/* DMULS.L then STS MACH: the high 32 bits of the product */
static int32_t multiply_high(int32_t a, int32_t b)
{
	return (int32_t)(((int64_t)a * b) >> 32);
}

/* The SH2's 64/32 divider as PicoDrive emulates it (pico/32x/sh2soc.c): rounds toward
   zero, and gives the largest value of the right sign when the quotient overflows. */
static int32_t divide(int64_t dividend, int32_t divisor)
{
	if (divisor == 0)
		return 0;
	int64_t quotient = dividend / divisor;
	if (quotient > INT32_MAX)
		return INT32_MAX;
	if (quotient < INT32_MIN)
		return INT32_MIN;
	return (int32_t)quotient;
}

/* ---- Memory: the scene, models in ROM and SDRAM ---- */

#define SDRAM_START 0x06000000
#define SDRAM_SIZE  0x40000
#define ROM_START   0x02000000
#define CACHE_THROUGH 0x20000000

/* Our working copy of the scene. The SH2s change their copy as they go (they clear the
   flags of objects they cull, and sort the track list), and so does this file. */
static uint8_t scene[HLE_SCENE_SIZE];

static uint16_t scene_word(int offset)
{
	return (uint16_t)(scene[offset] << 8 | scene[offset + 1]);
}

static void set_scene_word(int offset, uint16_t value)
{
	scene[offset] = value >> 8;
	scene[offset + 1] = value & 0xFF;
}

static uint32_t scene_long(int offset)
{
	return (uint32_t)scene_word(offset) << 16 | scene_word(offset + 2);
}

static void set_scene_long(int offset, uint32_t value)
{
	set_scene_word(offset, value >> 16);
	set_scene_word(offset + 2, value & 0xFFFF);
}

/* A 16-bit word at an SH2 address, where models live: the cartridge ROM, or SDRAM
   (models copied there from the ROM, and lists inside the scene). PicoDrive keeps both
   as 16-bit words in host byte order. */
static uint16_t sh2_word(uint32_t address)
{
	address &= ~CACHE_THROUGH;
	if (address >= SDRAM_START && address < SDRAM_START + SDRAM_SIZE) {
		uint32_t offset = address - SDRAM_START;
		uint32_t scene_offset = address - HLE_SCENE_ADDRESS;
		if (scene_offset < HLE_SCENE_SIZE)
			return scene_word(scene_offset);
		return ((const uint16_t *)Pico32xMem->sdram)[offset / 2];
	}
	if (address >= ROM_START && address - ROM_START < Pico.romsize)
		return ((const uint16_t *)Pico.rom)[(address - ROM_START) / 2];
	return 0;
}

static int16_t sh2_signed_word(uint32_t address)
{
	return (int16_t)sh2_word(address);
}

/* A long at a ROM offset */
static uint32_t rom_long(uint32_t offset)
{
	return (uint32_t)sh2_word(ROM_START + offset) << 16 | sh2_word(ROM_START + offset + 2);
}

void hle_read_scene(uint8_t *out)
{
	const uint16_t *sdram = (const uint16_t *)Pico32xMem->sdram;
	for (int i = 0; i < HLE_SCENE_SIZE / 2; i++) {
		uint16_t word = sdram[(HLE_SCENE_ADDRESS - SDRAM_START) / 2 + i];
		out[2 * i] = word >> 8;
		out[2 * i + 1] = word & 0xFF;
	}
}

/* ---- Scene layout (offsets from 0600C000) ---- */

#define TRACK_LIST          0x000   /* 32-bit model pointers, sorted by the master */
#define FOCAL_LENGTH        0x0CE   /* word: projection scale (0x70) */
#define TRACK_NEAR          0x0CC   /* word: near plane for the track, << 8 */
#define MIRROR              0x0D4   /* byte: non-zero = picture mirrored left-right */

/* Object records, 0x14 bytes each from +100 */
#define RECORDS_START       0x100
#define RECORD_SIZE         0x14
#define RECORD_COUNT        ((HLE_SCENE_SIZE - RECORDS_START) / RECORD_SIZE)
#define FLAG    0x00   /* word: 0 = not drawn */
#define POS_X   0x02   /* words: position */
#define POS_Y   0x04
#define POS_Z   0x06
#define ANGLE_X 0x08   /* words: rotation, 0x2000 = full turn */
#define ANGLE_Y 0x0A
#define ANGLE_Z 0x0C
#define EXTRA   0x0E   /* word: depends on the object (track segment count, scale, colour) */
#define MODEL   0x10   /* long: model address */

/* Records by role */
#define CAMERA          0x100
#define PLAYER_CAR      0x114
#define PLAYER_PARTS    0x128   /* 3 children (wheels), then one at +164 */
#define PLAYER_EXTRA    0x164
#define EFFECTS         0x178   /* 8: tyre smoke, grass, ... */
#define OPPONENTS       0x218   /* 15 cars of 3 records: body and two children */
#define OPPONENT_SIZE   (3 * RECORD_SIZE)
#define ROADSIDE        0x59C   /* 16 objects */
#define SIGNS           0x6DC   /* 4 records with lists of up to 4 models */
#define COURSE_MAP      0x72C   /* HUD: the course map */
#define NEEDLE          0x740   /* HUD: the rev counter needle */
#define HUD_MODEL       0x754   /* HUD: another HUD model */

static int16_t field(int record, int offset)
{
	return (int16_t)scene_word(record + offset);
}

/* Each scene word exactly: the word itself, except between two ticks (hle_interpolated),
   where positions and angles have fractions that the words hold rounded */
static double exact_word[HLE_SCENE_SIZE / 2];

/* A scene value as the SH2 reads it (i) and exactly (f) */
typedef struct {
	int32_t i;
	double f;
} value_t;

static value_t value(int record, int offset)
{
	value_t v = { field(record, offset), exact_word[(record + offset) / 2] };
	return v;
}

static value_t constant(int32_t i)
{
	value_t v = { i, i };
	return v;
}

static value_t negative(value_t v)
{
	value_t negated = { -v.i, -v.f };
	return negated;
}

static value_t sum(value_t a, value_t b)
{
	value_t total = { a.i + b.i, a.f + b.f };
	return total;
}

static int record_index(int record)
{
	return (record - RECORDS_START) / RECORD_SIZE;
}

/* ---- Matrices (master SH2, 06001A00-06002298) ----

   A matrix is 3 rows of (m0, m1, m2, translation), 16.16 fixed point. Rotations and
   translations are applied on the right (in the object's own frame).

   Each matrix is kept twice: m as the SH2 computes it, and f with the same operations
   done exactly (doubles, real sine and cosine, nothing rounded), in units where 1.0 is
   0x10000 in m. m decides everything the SH2s decide (what is drawn, clipped, culled, in
   which order); f gives the vertices' positions for drawing at a high resolution
   without the SH2's rounding (hle_polygon_t's fx, fy). */

typedef struct {
	int32_t m[3][4];
	double f[3][4];
} matrix_t;

#define FIXED_ONE 65536.0   /* 1.0 in 16.16 */

/* Records drawn with what the SH2s would not draw (detailed opponents, the far view's
   props): their polygons take none of the SH2s' room */
static uint8_t record_extra[RECORD_COUNT];

/* One matrix per object record (SDRAM 0600CA00, 0x30 bytes each) */
static matrix_t object_matrix[RECORD_COUNT];

static matrix_t identity(void)
{
	matrix_t matrix = {
		{ { 0x10000, 0, 0, 0 }, { 0, 0x10000, 0, 0 }, { 0, 0, 0x10000, 0 } },
		{ { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } },
	};
	return matrix;
}

/* The exact sine and cosine (the ROM's table is a sine to within one unit) */
static double exact_sine(double angle)
{
	return sin(angle * (2 * M_PI / 0x2000));
}

static double exact_cosine(double angle)
{
	return cos(angle * (2 * M_PI / 0x2000));
}

/* Exact rotation of columns a and b by (c, s): a' = a c + b s1, b' = a s2 + b c */
static void rotate_exact(matrix_t *matrix, int a, int b, double c, double s1, double s2)
{
	for (int i = 0; i < 3; i++) {
		double fa = matrix->f[i][a], fb = matrix->f[i][b];
		matrix->f[i][a] = fa * c + fb * s1;
		matrix->f[i][b] = fa * s2 + fb * c;
	}
}

/* 06002286: sine and cosine from the ROM table at 025B84 (0x4000 = 1.0), as 16.16.
   The angle is a byte offset into the table: 0x2000 is a full turn, bit 0 is ignored. */
static int32_t sine(int32_t angle)
{
	return (int32_t)sh2_signed_word(ROM_START + 0x025B84 + (angle & 0x1FFE)) << 2;
}

static int32_t cosine(int32_t angle)
{
	return sine(angle + 0x800);
}

/* 060021C6 */
static void rotate_x(matrix_t *matrix, value_t angle)
{
	int32_t s = sine(angle.i), c = cosine(angle.i);
	for (int i = 0; i < 3; i++) {
		int32_t m1 = matrix->m[i][1], m2 = matrix->m[i][2];
		matrix->m[i][1] = (int32_t)(((int64_t)m1 * c + (int64_t)m2 * -s) >> 16);
		matrix->m[i][2] = (int32_t)(((int64_t)m1 * s + (int64_t)m2 * c) >> 16);
	}
	rotate_exact(matrix, 1, 2, exact_cosine(angle.f), -exact_sine(angle.f), exact_sine(angle.f));
}

/* 06002206 */
static void rotate_y(matrix_t *matrix, value_t angle)
{
	int32_t s = sine(angle.i), c = cosine(angle.i);
	for (int i = 0; i < 3; i++) {
		int32_t m0 = matrix->m[i][0], m2 = matrix->m[i][2];
		matrix->m[i][0] = (int32_t)(((int64_t)m0 * c + (int64_t)m2 * s) >> 16);
		matrix->m[i][2] = (int32_t)(((int64_t)m0 * -s + (int64_t)m2 * c) >> 16);
	}
	rotate_exact(matrix, 0, 2, exact_cosine(angle.f), exact_sine(angle.f), -exact_sine(angle.f));
}

/* 06002248 */
static void rotate_z(matrix_t *matrix, value_t angle)
{
	int32_t s = sine(angle.i), c = cosine(angle.i);
	for (int i = 0; i < 3; i++) {
		int32_t m0 = matrix->m[i][0], m1 = matrix->m[i][1];
		matrix->m[i][0] = (int32_t)(((int64_t)m0 * c + (int64_t)m1 * -s) >> 16);
		matrix->m[i][1] = (int32_t)(((int64_t)m0 * s + (int64_t)m1 * c) >> 16);
	}
	rotate_exact(matrix, 0, 1, exact_cosine(angle.f), -exact_sine(angle.f), exact_sine(angle.f));
}

static double exact_dot3(const double *a, const double *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* 06002006, with a separate exact vector (in FIXED_ONE units) */
static void translate_both(matrix_t *matrix, const int32_t v[3], const double fv[3])
{
	for (int i = 0; i < 3; i++) {
		matrix->m[i][3] += dot3(matrix->m[i], v);
		matrix->f[i][3] += exact_dot3(matrix->f[i], fv);
	}
}

/* 060020F0: translate along z only */
static void translate_z(matrix_t *matrix, int32_t z)
{
	for (int i = 0; i < 3; i++) {
		matrix->m[i][3] += multiply_16(matrix->m[i][2], z);
		matrix->f[i][3] += matrix->f[i][2] * (z / FIXED_ONE);
	}
}

/* 0600213C: scale the first column */
static void scale_x(matrix_t *matrix, int32_t scale)
{
	for (int i = 0; i < 3; i++) {
		matrix->m[i][0] = multiply_16(scale, matrix->m[i][0]);
		matrix->f[i][0] *= scale / FIXED_ONE;
	}
}

/* 0600210C: scale all three columns */
static void scale_all(matrix_t *matrix, int32_t scale)
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			matrix->m[i][j] = multiply_16(matrix->m[i][j], scale);
			matrix->f[i][j] *= scale / FIXED_ONE;
		}
}

/* 0600218A: mirror left-right */
static void negate_x(matrix_t *matrix)
{
	for (int i = 0; i < 3; i++) {
		matrix->m[i][0] = -matrix->m[i][0];
		matrix->f[i][0] = -matrix->f[i][0];
	}
}

/* A record's position word as 16.16 (shifted left), and exactly in FIXED_ONE units */
static int32_t position(int record, int offset, int shift)
{
	return field(record, offset) << shift;
}

static double exact_position(int record, int offset, int shift)
{
	return value(record, offset).f * (1 << shift) / FIXED_ONE;
}

/* Positions in records: x and z are in units 16 times smaller than y (06001AC4). */
static void translate_to_world_position(matrix_t *matrix, int record)
{
	int32_t v[3] = { position(record, POS_X, 12), position(record, POS_Y, 8), position(record, POS_Z, 12) };
	double fv[3] = { exact_position(record, POS_X, 12), exact_position(record, POS_Y, 8),
		exact_position(record, POS_Z, 12) };
	translate_both(matrix, v, fv);
}

/* Children are placed relative to their parent, all axes in y units (06001B1C). */
static void translate_to_local_position(matrix_t *matrix, int record)
{
	int32_t v[3] = { position(record, POS_X, 8), position(record, POS_Y, 8), position(record, POS_Z, 8) };
	double fv[3] = { exact_position(record, POS_X, 8), exact_position(record, POS_Y, 8),
		exact_position(record, POS_Z, 8) };
	translate_both(matrix, v, fv);
}

/* 06001F3A: objects behind the camera, further than z = 200, or more than 100 to the
   side are not drawn. Only the integer part of x is compared. */
static int outside_view(const matrix_t *matrix)
{
	int32_t z = matrix->m[2][3];
	if (z <= 0 || z >= (200 << 16))
		return 1;
	int16_t x = (int16_t)(matrix->m[0][3] >> 16);
	if (x < 0)
		x = -x;
	return x >= 100;
}

static void clear_flag(int record)
{
	set_scene_word(record + FLAG, 0);
}

static int in_far_view(const matrix_t *matrix);
static int in_far_window(int record);

static int mirrored(void)
{
	return scene[MIRROR] != 0;
}

/* 06001A6C: the camera's world position, 16.16 with x and z in world units / 16, and
   exactly (in FIXED_ONE units) */
static void camera_position(int32_t position[3], double exact[3])
{
	if (field(CAMERA, FLAG) == 2) {
		static const int offsets[3] = { POS_X, POS_Y, POS_Z }, shifts[3] = { 12, 8, 12 };
		for (int i = 0; i < 3; i++) {
			position[i] = field(CAMERA, offsets[i]) << shifts[i];
			exact[i] = exact_position(CAMERA, offsets[i], shifts[i]);
		}
		return;
	}
	/* Otherwise the camera record holds an offset from the player's car, turned by the
	   camera's angles (06001B5E). */
	int32_t car[3] = { field(PLAYER_CAR, POS_X) << 12, field(PLAYER_CAR, POS_Y) << 8, field(PLAYER_CAR, POS_Z) << 12 };
	double exact_car[3] = { exact_position(PLAYER_CAR, POS_X, 12), exact_position(PLAYER_CAR, POS_Y, 8),
		exact_position(PLAYER_CAR, POS_Z, 12) };
	int32_t offset[3] = { field(CAMERA, POS_X) << 8, field(CAMERA, POS_Y) << 8, -(field(CAMERA, POS_Z) << 8) };
	double exact_offset[3] = { exact_position(CAMERA, POS_X, 8), exact_position(CAMERA, POS_Y, 8),
		-exact_position(CAMERA, POS_Z, 8) };
	matrix_t turn = identity();
	rotate_y(&turn, negative(value(CAMERA, ANGLE_Y)));
	rotate_x(&turn, negative(value(CAMERA, ANGLE_X)));
	rotate_z(&turn, negative(value(CAMERA, ANGLE_Z)));
	for (int i = 0; i < 3; i++) {
		position[i] = dot3(turn.m[i], offset) + car[i];
		exact[i] = exact_dot3(turn.f[i], exact_offset) + exact_car[i];
	}
}

/* 06001BD4: world to camera space */
static matrix_t view_matrix(void)
{
	int32_t position[3];
	double exact[3];
	camera_position(position, exact);

	matrix_t view = identity();
	if (mirrored())
		negate_x(&view);
	rotate_z(&view, value(CAMERA, ANGLE_Z));
	rotate_x(&view, value(CAMERA, ANGLE_X));
	rotate_y(&view, value(CAMERA, ANGLE_Y));
	int32_t back[3] = { -position[0], -position[1], -position[2] };
	double exact_back[3] = { -exact[0], -exact[1], -exact[2] };
	translate_both(&view, back, exact_back);
	return view;
}

/* 06001C08: the player's car, its wheels and effects */
static void player_car_matrices(const matrix_t *view)
{
	int flag = field(PLAYER_CAR, FLAG);
	if (flag == 0)
		return;
	if (flag == 2)
		return;   /* handled by cockpit_matrices */

	matrix_t car = *view;
	translate_to_world_position(&car, PLAYER_CAR);
	rotate_y(&car, value(PLAYER_CAR, ANGLE_Y));
	rotate_x(&car, value(PLAYER_CAR, ANGLE_X));
	rotate_z(&car, value(PLAYER_CAR, ANGLE_Z));
	object_matrix[record_index(PLAYER_CAR)] = car;

	for (int i = 0; i < 3; i++) {
		int part = PLAYER_PARTS + i * RECORD_SIZE;
		if (field(part, FLAG) == 0)
			continue;
		matrix_t matrix = car;
		translate_to_local_position(&matrix, part);
		rotate_y(&matrix, value(part, ANGLE_Y));
		rotate_x(&matrix, value(part, ANGLE_X));
		object_matrix[record_index(part)] = matrix;
	}
	object_matrix[record_index(PLAYER_EXTRA)] = car;   /* whatever its flag */

	for (int i = 0; i < 8; i++) {
		int effect = EFFECTS + i * RECORD_SIZE;
		int effect_flag = field(effect, FLAG);
		if (effect_flag == 0)
			continue;
		matrix_t matrix = car;
		if (effect_flag == 2)
			negate_x(&matrix);
		translate_to_local_position(&matrix, effect);
		object_matrix[record_index(effect)] = matrix;
	}
}

/* 06001C96: the player's car with flag 2 (cockpit view): placed relative to the camera,
   not the world. Only two wheels; no effects. */
static void cockpit_matrices(void)
{
	if (field(PLAYER_CAR, FLAG) != 2)
		return;

	matrix_t car = identity();
	if (mirrored())
		negate_x(&car);
	int32_t to_camera[3] = { 0, -(field(CAMERA, POS_Y) << 8), field(CAMERA, POS_Z) << 8 };
	double exact_to_camera[3] = { 0, -exact_position(CAMERA, POS_Y, 8), exact_position(CAMERA, POS_Z, 8) };
	translate_both(&car, to_camera, exact_to_camera);
	rotate_x(&car, sum(value(CAMERA, ANGLE_X), value(PLAYER_CAR, ANGLE_X)));
	matrix_t before_roll = car;
	rotate_z(&car, negative(sum(value(CAMERA, ANGLE_Z), value(PLAYER_CAR, ANGLE_Z))));
	object_matrix[record_index(PLAYER_CAR)] = car;

	/* The wheels hang off the matrix as it was before the roll (the SH2 keeps that copy
	   on its matrix stack). */
	for (int i = 0; i < 2; i++) {
		int part = PLAYER_PARTS + i * RECORD_SIZE;
		if (field(part, FLAG) == 0)
			continue;
		matrix_t matrix = before_roll;
		translate_to_local_position(&matrix, part);
		rotate_y(&matrix, value(part, ANGLE_Y));
		rotate_x(&matrix, value(part, ANGLE_X));
		object_matrix[record_index(part)] = matrix;
	}

	if (field(PLAYER_EXTRA, FLAG) != 0) {
		translate_to_local_position(&car, PLAYER_EXTRA);
		rotate_z(&car, value(PLAYER_EXTRA, ANGLE_Z));
		object_matrix[record_index(PLAYER_EXTRA)] = car;
	}
}

/* 06001D34: an opponent's car and its two wheel records */
static void opponent_matrices(const matrix_t *view, int body)
{
	if (field(body, FLAG) == 0)
		return;

	matrix_t car = *view;
	translate_to_world_position(&car, body);
	rotate_y(&car, value(body, ANGLE_Y));
	rotate_x(&car, value(body, ANGLE_X));
	rotate_z(&car, value(body, ANGLE_Z));
	if (outside_view(&car)) {
		/* The SH2 clears the body's flag word and a long at each child's +0 (its flag
		   and x). */
		clear_flag(body);
		set_scene_long(body + RECORD_SIZE, 0);
		set_scene_long(body + 2 * RECORD_SIZE, 0);
		return;
	}
	object_matrix[record_index(body)] = car;

	for (int i = 1; i <= 2; i++) {
		int part = body + i * RECORD_SIZE;
		matrix_t matrix = car;
		translate_to_local_position(&matrix, part);
		rotate_x(&matrix, value(part, ANGLE_X));
		object_matrix[record_index(part)] = matrix;
	}
}

/* 06001E9C: roadside objects. Returns the number of records used (flag 2 objects
   have a child record). */
static int roadside_matrices(const matrix_t *view, int record)
{
	int flag = field(record, FLAG);
	if (flag == 1) {
		matrix_t matrix = *view;
		translate_to_world_position(&matrix, record);
		rotate_y(&matrix, value(record, ANGLE_Y));
		scale_x(&matrix, field(record, EXTRA) << 8);
		if (outside_view(&matrix)) {
			if (!in_far_view(&matrix) || !in_far_window(record)) {
				clear_flag(record);
				return 1;
			}
			record_extra[record_index(record)] = 1;
		}
		object_matrix[record_index(record)] = matrix;
		return 1;
	}
	if (flag == 2) {
		int child = record + RECORD_SIZE;
		matrix_t matrix = *view;
		translate_to_world_position(&matrix, record);
		rotate_y(&matrix, value(record, ANGLE_Y));
		if (outside_view(&matrix)) {
			if (!in_far_view(&matrix) || !in_far_window(record)) {
				clear_flag(record);
				clear_flag(child);
				return 2;
			}
			record_extra[record_index(record)] = 1;
			record_extra[record_index(child)] = 1;
		}
		object_matrix[record_index(record)] = matrix;
		translate_to_local_position(&matrix, child);
		rotate_z(&matrix, value(child, ANGLE_Z));
		object_matrix[record_index(child)] = matrix;
		return 2;
	}
	return 1;   /* 0 and others: nothing */
}

/* 06001DA4: signs (no rotation) */
static void sign_matrix(const matrix_t *view, int record)
{
	if (field(record, FLAG) == 0)
		return;
	matrix_t matrix = *view;
	translate_to_world_position(&matrix, record);
	if (outside_view(&matrix))
		clear_flag(record);
	else
		object_matrix[record_index(record)] = matrix;
}

/* 06001E7E, 06001DCC, 06001DF8: the HUD models */
static void hud_matrices(void)
{
	if (field(HUD_MODEL, FLAG) != 0) {
		matrix_t matrix = identity();
		translate_to_local_position(&matrix, HUD_MODEL);
		rotate_x(&matrix, value(HUD_MODEL, ANGLE_X));
		object_matrix[record_index(HUD_MODEL)] = matrix;
	}
	if (field(NEEDLE, FLAG) != 0) {
		matrix_t matrix = identity();
		translate_z(&matrix, 0x10000);
		rotate_z(&matrix, value(NEEDLE, ANGLE_Z));
		scale_all(&matrix, field(NEEDLE, EXTRA) << 8);
		object_matrix[record_index(NEEDLE)] = matrix;
	}
	if (field(COURSE_MAP, FLAG) != 0) {
		/* Looked at from above, turned with the camera, centred on the player's car */
		matrix_t matrix = identity();
		if (mirrored())
			negate_x(&matrix);
		rotate_x(&matrix, constant(0x800));
		rotate_y(&matrix, value(CAMERA, ANGLE_Y));
		int32_t centre[3] = { -(field(PLAYER_CAR, POS_X) << 12), field(COURSE_MAP, POS_Z) << 8,
			-(field(PLAYER_CAR, POS_Z) << 12) };
		double exact_centre[3] = { -exact_position(PLAYER_CAR, POS_X, 12), exact_position(COURSE_MAP, POS_Z, 8),
			-exact_position(PLAYER_CAR, POS_Z, 12) };
		translate_both(&matrix, centre, exact_centre);
		object_matrix[record_index(COURSE_MAP)] = matrix;
	}
}

/* 06000DC8 */
static void build_matrices(void)
{
	matrix_t view = view_matrix();
	object_matrix[record_index(CAMERA)] = view;

	player_car_matrices(&view);
	cockpit_matrices();
	for (int i = 0; i < 15; i++)
		opponent_matrices(&view, OPPONENTS + i * OPPONENT_SIZE);
	int record = ROADSIDE;
	for (int i = 0; i < 16; i++)
		record += roadside_matrices(&view, record) * RECORD_SIZE;
	for (int i = 0; i < 4; i++)
		sign_matrix(&view, SIGNS + i * RECORD_SIZE);
	hud_matrices();
}

/* 0600115C: sorts the track segments by their models' first word (vertex count), most
   first, so that the two SH2s get similar amounts of work. A bubble sort; equal ones
   keep their order. */
static void sort_track_segments(void)
{
	int count = field(CAMERA, EXTRA);
	uint16_t key[64];
	if (count > 64)
		count = 64;
	for (int i = 0; i < count; i++)
		key[i] = sh2_word(scene_long(TRACK_LIST + 4 * i));

	for (int pass = count - 1; pass > 0; pass--) {
		for (int i = 0; i < pass; i++) {
			if ((int16_t)key[i] >= (int16_t)key[i + 1])
				continue;
			uint16_t swap_key = key[i];
			key[i] = key[i + 1];
			key[i + 1] = swap_key;
			uint32_t swap_model = scene_long(TRACK_LIST + 4 * i);
			set_scene_long(TRACK_LIST + 4 * i, scene_long(TRACK_LIST + 4 * (i + 1)));
			set_scene_long(TRACK_LIST + 4 * (i + 1), swap_model);
		}
	}
}

/* ---- Geometry (code overlay A, both SH2s) ---- */

typedef struct {
	int32_t x, y, z;        /* camera space, 16.16 */
	int16_t sx, sy;         /* projected, relative to the screen centre, y up */
	double fx, fy, fz;      /* the same exactly, from the matrices' f (FIXED_ONE units) */
	float fsx, fsy;
} vertex_t;

/* Depth buckets (SDRAM 0600DB00: one count byte per bucket and SH2; entries from
   06020000). Buckets 0-511 cover z 0-16 in steps of 1/32, buckets 512-2431 z 16-255 in
   steps of 1/8. Each SH2 has room for 8 polygons per bucket. The polygons only exact
   vertices draw (HLE_EXACT_VERTICES_ONLY) go in as well, but take none of that room, so
   that the SH2s' polygons stay the same. So do the far view's (see "Far view"), which
   also has buckets past z 255, in steps of 1. */
#define BUCKET_COUNT 0x980
#define BUCKET_ROOM  8
#define BUCKET_ENTRIES (4 * BUCKET_ROOM)   /* room for the exact-only and far ones as well */
#define FAR_LIMIT      0x00FF0000   /* z and depth limit, 255 (+10 in the parameter block) */
#define FAR_VIEW_LIMIT 0x02000000   /* the same in far view by default: 512, twice as far */
#define FAR_VIEW_MOST  0x04000000   /* the furthest it can be set to (1024) */
#define ALL_BUCKETS    (BUCKET_COUNT + ((FAR_VIEW_MOST - FAR_LIMIT) >> 16))

typedef struct {
	int count[ALL_BUCKETS][2];                   /* the SH2s' polygons */
	int entries[ALL_BUCKETS][2];                 /* with the exact-only and far ones */
	int entry[ALL_BUCKETS][2][BUCKET_ENTRIES];   /* polygon numbers, in the order put in */
} buckets_t;

static buckets_t buckets;
static hle_polygon_t pool[HLE_MAX_POLYGONS];   /* every polygon the geometry made */
static int pool_count;

#define MAX_VERTICES 1024

/* One SH2's geometry state: the parameter block at C0000700 and its buffers */
typedef struct {
	int half;                       /* 0 = master, 1 = slave: its share of each bucket */
	int32_t focal;                  /* +08: projection scale (0x70) */
	int32_t near;                   /* +0C: near plane, 16.16 */
	int32_t near_scale;             /* +18: focal / near, for points on the near plane */
	int vertex_shift;               /* +1C: model units to 16.16 (6, or 11 for the track) */
	uint32_t record_address;        /* +24: where the next polygon record goes */
	uint32_t record_limit;          /* +28 */
	matrix_t matrix;                /* C0000740: the object's matrix */
	uint16_t colour;                /* +02: the polygon's colour word */
	int32_t normal[3];              /* C000077C */
	vertex_t vertices[MAX_VERTICES];/* +2C: the model's transformed vertices */
	vertex_t slots[10];             /* +20: the polygon being built; 5-9 = copy for clipping */
	int base;                       /* 0, or 5 while clipping */
	int extra;                      /* drawing track cells the 68000 did not send */
	int beyond_sh2;                 /* the polygon is one the SH2s leave out (far view) */
} geometry_t;

#define SIDE_LIMIT     0x00800000   /* +14: |x| limit, 128 */

/* Far view: more of the track, further away (see "Far view" below) */
static int far_view;
static int32_t far_view_limit = FAR_VIEW_LIMIT;
static int settings_version;   /* counts changes to the settings below (hle_interpolated) */
static int far_view_tiles = 8;   /* the cells it adds: so many steps from the car's (walk_tiles) */

void hle_set_far_view(int on)
{
	settings_version++;
	far_view = on;
}

void hle_set_far_view_distance(int world_units)
{
	settings_version++;
	int32_t limit = world_units << 12;   /* FIXED_ONE is 16 world units */
	far_view_limit = limit < FAR_LIMIT ? FAR_LIMIT : limit > FAR_VIEW_MOST ? FAR_VIEW_MOST : limit;
}

void hle_set_far_view_tiles(int tiles)
{
	settings_version++;
	far_view_tiles = tiles;
}

/* How far to the side the far view looks, in 256ths of the depth (hle_set_side_reach) */
static int side_reach = HLE_SIDE_REACH_16_9;

void hle_set_side_reach(int reach)
{
	settings_version++;
	side_reach = reach < HLE_SIDE_REACH_16_9 ? HLE_SIDE_REACH_16_9 : reach;
}

/* Far view: an object whose centre the SH2s leave out (06001F3A) but that is in sight
   up to the far view's end: within side_reach of its depth to each side, as the track's
   cells, with a margin for its size */
#define OBJECT_MARGIN 0x00400000   /* 64, 1024 world units */

static int in_far_view(const matrix_t *matrix)
{
	int32_t x = abs(matrix->m[0][3]), z = matrix->m[2][3];
	return far_view && z > 0 && z < far_view_limit && x < (int64_t)z * side_reach / 256 + OBJECT_MARGIN;
}
static const int32_t light[3] = { 0, -0x40000, 0 };   /* C0000788 */

static void set_near(geometry_t *g, int32_t near)
{
	g->near = near;
	g->near_scale = divide((int64_t)g->focal << 32, near);   /* 06002398 */
}

/* The exact camera-space position of a model vertex (v as the SH2 loads it, 16.16) */
static void transform_exact_vector(const matrix_t *matrix, const double fv[3], vertex_t *out)
{
	out->fx = exact_dot3(matrix->f[0], fv) + matrix->f[0][3];
	out->fy = exact_dot3(matrix->f[1], fv) + matrix->f[1][3];
	out->fz = exact_dot3(matrix->f[2], fv) + matrix->f[2][3];
}

static void transform_exact(const matrix_t *matrix, const int32_t v[3], vertex_t *out)
{
	double fv[3] = { v[0] / FIXED_ONE, v[1] / FIXED_ONE, v[2] / FIXED_ONE };
	transform_exact_vector(matrix, fv, out);
}

/* The exact projection. The SH2 decided the vertex is in front of the near plane; if the
   exact z disagrees that far (it never should), the SH2's position is kept. */
static void project_exact(int32_t focal, vertex_t *v)
{
	if (v->fz <= 0) {
		v->fsx = v->sx;
		v->fsy = v->sy;
		return;
	}
	v->fsx = (float)(focal * v->fx / v->fz);
	v->fsy = (float)(focal * v->fy / v->fz);
}

/* C0000128: transform (and, when in front of the near plane, project) the vertices */
static void transform_vertices(geometry_t *g, uint32_t *model, int count)
{
	if (count > MAX_VERTICES) {
		fprintf(stderr, "hle: model with %d vertices\n", count);
		count = MAX_VERTICES;
	}
	for (int i = 0; i < count; i++) {
		int32_t v[3];
		for (int j = 0; j < 3; j++)
			v[j] = (int32_t)sh2_signed_word(*model + 2 * j) << g->vertex_shift;
		*model += 6;

		vertex_t *out = &g->vertices[i];
		out->z = dot3(g->matrix.m[2], v) + g->matrix.m[2][3];
		out->x = dot3(g->matrix.m[0], v) + g->matrix.m[0][3];
		out->y = dot3(g->matrix.m[1], v) + g->matrix.m[1][3];
		transform_exact(&g->matrix, v, out);
		if (g->near > out->z)
			continue;   /* behind the near plane: keeps its old screen position */
		int32_t scale = divide((int64_t)g->focal << 32, out->z);
		out->sx = (int16_t)multiply_high(scale, out->x);
		out->sy = (int16_t)multiply_high(scale, out->y);
		project_exact(g->focal, out);
	}
}

static vertex_t *slot(geometry_t *g, int n)
{
	return &g->slots[g->base + n];
}

/* Far view keeps a vertex up to far_view_limit away, and to the side as long as it is
   within twice side_reach of its distance (4 times for 16:9: well outside the picture) */
static int outside_far_view(const vertex_t *v)
{
	int32_t x = v->x < 0 ? -v->x : v->x;
	return v->z > far_view_limit || (x > SIDE_LIMIT && (int64_t)x * 128 / side_reach > v->z);
}

/* C00001EC: 1 if the polygon is rejected outright (a vertex too far to the side or too
   far away); otherwise *behind gets bit n+1 set for each vertex n behind the near plane.
   In far view such a polygon may still be drawn, as one beyond the SH2s. */
static int outcode(geometry_t *g, int vertex_count, int *behind)
{
	*behind = 0;
	g->beyond_sh2 = g->extra;
	for (int n = 0; n < vertex_count; n++) {
		const vertex_t *v = slot(g, n);
		int32_t x = v->x < 0 ? -v->x : v->x;
		if (x > SIDE_LIMIT || v->z > FAR_LIMIT) {
			if (!far_view || outside_far_view(v))
				return 1;
			g->beyond_sh2 = 1;
		}
		if (g->near >= v->z)
			*behind |= 2 << n;
	}
	return 0;
}

/* C000030C: front-facing test on the first three projected vertices */
static int faces_camera(geometry_t *g)
{
	const vertex_t *v0 = slot(g, 0), *v1 = slot(g, 1), *v2 = slot(g, 2);
	int64_t sum = (int64_t)(int16_t)(v1->sx - v0->sx) * (int16_t)(v2->sy - v1->sy)
		+ (int64_t)(int16_t)(v0->sy - v1->sy) * (int16_t)(v2->sx - v1->sx);
	int32_t cross = (int32_t)sum;   /* MULS.W and MAC.W keep 32 bits */
	if (mirrored())
		cross = -cross;   /* 06002C20 patches the overlay */
	return cross > 0;
}

/* The same test on the exact screen positions */
static int faces_camera_exact(geometry_t *g)
{
	const vertex_t *v0 = slot(g, 0), *v1 = slot(g, 1), *v2 = slot(g, 2);
	double cross = (double)(v1->fsx - v0->fsx) * (v2->fsy - v1->fsy)
		+ (double)(v0->fsy - v1->fsy) * (v2->fsx - v1->fsx);
	if (mirrored())
		cross = -cross;
	return cross > 0;
}

static int32_t minimum(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t maximum(int32_t a, int32_t b) { return a > b ? a : b; }

/* C0000340 (quads) and C00003AE (triangles): the depth used for sorting. Header bits
   1-3: 0 average, 2 nearest, 4 farthest, 6/8/A farthest plus 24, 16 or 36. */
static int32_t sort_depth(geometry_t *g, uint16_t header, int vertex_count)
{
	int32_t z0 = slot(g, 0)->z, z1 = slot(g, 1)->z, z2 = slot(g, 2)->z, z3 = slot(g, 3)->z;
	int32_t nearest = minimum(minimum(z0, z1), z2);
	int32_t farthest = maximum(maximum(z0, z1), z2);
	if (vertex_count == 4) {
		nearest = minimum(nearest, z3);
		farthest = maximum(farthest, z3);
	}
	switch (header & 0x0E) {
	case 0x00:
		if (vertex_count == 4)
			return (z0 + z1 + z2 + z3) >> 2;
		return multiply_16(z0 + z1 + z2, 0x5555);
	case 0x02: return nearest;
	case 0x04: return farthest;
	case 0x06: return farthest + 0x180000;
	case 0x08: return farthest + 0x100000;
	case 0x0A: return farthest + 0x240000;
	}
	fprintf(stderr, "hle: depth mode %x\n", header & 0x0E);
	return farthest;
}

/* C0000400: puts the polygon in its depth bucket, or the next one if that is full.
   Returns 0 if both are full. A polygon the SH2s did not draw goes in its own bucket
   without counting towards the room. */
static int put_in_bucket(geometry_t *g, int32_t depth, int polygon, int sh2_drew)
{
	int bucket;
	if (depth < 0x100000)
		bucket = (uint32_t)depth >> 11;                 /* z < 16: steps of 1/32 */
	else if (depth < FAR_LIMIT)
		bucket = 0x180 + ((uint32_t)depth >> 13);       /* steps of 1/8 */
	else
		bucket = BUCKET_COUNT + ((depth - FAR_LIMIT) >> 16);   /* far view: steps of 1 */
	for (int tries = 0; tries < 2; tries++, bucket++) {
		if (bucket >= (sh2_drew ? BUCKET_COUNT : ALL_BUCKETS))
			return 0;
		int *count = &buckets.count[bucket][g->half];
		int *entries = &buckets.entries[bucket][g->half];
		if (sh2_drew && *count == BUCKET_ROOM)
			continue;
		if (!sh2_drew && *entries - *count == BUCKET_ENTRIES - BUCKET_ROOM)
			return 0;   /* the exact-only ones have their own room */
		buckets.entry[bucket][g->half][(*entries)++] = polygon;
		if (sh2_drew)
			(*count)++;
		return 1;
	}
	return 0;
}

/* C0000668: the colour word darkened or lightened by the light coming from above */
static uint16_t lit_colour(geometry_t *g, uint16_t colour)
{
	int32_t normal[3];
	for (int i = 0; i < 3; i++)
		normal[i] = dot3(g->matrix.m[i], g->normal);
	int64_t sum = (int64_t)normal[0] * light[0] + (int64_t)normal[1] * light[1] + (int64_t)normal[2] * light[2];
	int32_t shade = (int32_t)(sum >> 32);
	if (shade < -4)
		shade = -4;
	if (shade > 3)
		shade = 3;
	return (uint16_t)(colour + (shade << 8));
}

/* C0000286 (quads) and C00002CC (triangles): records the polygon in the current slots */
static void emit(geometry_t *g, uint16_t header, int vertex_count)
{
	int drawn_with = HLE_BOTH_VERTICES;
	if (header & 0x10) {   /* drawn only when facing the camera */
		int sh2_facing = faces_camera(g), exact_facing = faces_camera_exact(g);
		if (!sh2_facing && !exact_facing)
			return;
		if (!sh2_facing)
			drawn_with = HLE_EXACT_VERTICES_ONLY;
		else if (!exact_facing)
			drawn_with = HLE_SH2_VERTICES_ONLY;
	}
	int sh2_drew = drawn_with != HLE_EXACT_VERTICES_ONLY && !g->beyond_sh2;
	int32_t depth = sort_depth(g, header, vertex_count);
	if (depth >= FAR_LIMIT) {
		if (!far_view || depth >= far_view_limit)
			return;
		sh2_drew = 0;   /* the SH2s leave it out */
	}
	if (pool_count == HLE_MAX_POLYGONS)
		return;
	if (!put_in_bucket(g, depth, pool_count, sh2_drew))
		return;

	uint16_t colour = g->colour;
	if (header & 0x20)
		colour = lit_colour(g, colour);
	if (vertex_count == 4)
		colour &= ~1;
	else
		colour |= 1;

	hle_polygon_t *polygon = &pool[pool_count++];
	polygon->hud_anchor = 0;
	polygon->colour = colour;
	polygon->vertex_count = vertex_count;
	polygon->drawn_with = drawn_with;
	for (int n = 0; n < vertex_count; n++) {
		polygon->x[n] = slot(g, n)->sx;
		polygon->y[n] = slot(g, n)->sy;
		polygon->fx[n] = slot(g, n)->fsx;
		polygon->fy[n] = slot(g, n)->fsy;
		polygon->depth[n] = (float)(slot(g, n)->fz * 16);   /* FIXED_ONE is 16 units of x and z */
	}
	if (sh2_drew)
		g->record_address += vertex_count == 4 ? 0x14 : 0x10;
}

/* C000060C: the point where the edge from slot a to slot b crosses the near plane, into
   slot out. Only z and the screen position are set. */
static void near_point(geometry_t *g, int a, int b, int out)
{
	const vertex_t *va = slot(g, a), *vb = slot(g, b);
	int32_t t = divide((int64_t)(g->near - vb->z) << 16, vb->z - va->z);
	int32_t x = multiply_16(t, vb->x - va->x) + vb->x;
	int32_t y = multiply_16(t, vb->y - va->y) + vb->y;
	vertex_t *o = slot(g, out);
	o->z = g->near;
	o->sx = (int16_t)multiply_high(g->near_scale, x);
	o->sy = (int16_t)multiply_high(g->near_scale, y);

	/* The same point exactly. The SH2 decided that a and b lie on either side of the
	   near plane, so t is between -1 and 0 (from b towards a). */
	double near = g->near / FIXED_ONE;
	double dz = vb->fz - va->fz;
	double ft = dz != 0 ? (near - vb->fz) / dz : 0;
	ft = ft < -1 ? -1 : ft > 0 ? 0 : ft;
	o->fz = near;
	o->fsx = (float)(g->focal * (vb->fx + (vb->fx - va->fx) * ft) / near);
	o->fsy = (float)(g->focal * (vb->fy + (vb->fy - va->fy) * ft) / near);
}

/* C00005FE: copies z and the screen position */
static void copy_point(geometry_t *g, int from, int to)
{
	vertex_t *t = slot(g, to);
	const vertex_t *f = slot(g, from);
	t->z = f->z;
	t->sx = f->sx;
	t->sy = f->sy;
	t->fz = f->fz;
	t->fsx = f->fsx;
	t->fsy = f->fsy;
}

/* C0000220: a quad with vertices behind the near plane (bit n+1 of behind for vertex n).
   The part in front becomes a quad, a triangle, or a quad and a triangle. */
static void clip_quad(geometry_t *g, uint16_t header, int behind)
{
	switch (behind) {
	case 2:  near_point(g, 0, 1, 4); near_point(g, 0, 3, 0); emit(g, header, 4);
	         copy_point(g, 1, 2); copy_point(g, 4, 1); emit(g, header, 3); break;
	case 4:  near_point(g, 1, 2, 4); near_point(g, 1, 0, 1); emit(g, header, 4);
	         copy_point(g, 1, 0); copy_point(g, 4, 1); emit(g, header, 3); break;
	case 6:  near_point(g, 1, 2, 1); near_point(g, 0, 3, 0); emit(g, header, 4); break;
	case 8:  near_point(g, 2, 3, 4); near_point(g, 2, 1, 2); emit(g, header, 4);
	         copy_point(g, 3, 1); copy_point(g, 4, 0); emit(g, header, 3); break;
	case 12: near_point(g, 2, 3, 2); near_point(g, 1, 0, 1); emit(g, header, 4); break;
	case 14: near_point(g, 2, 3, 1); near_point(g, 0, 3, 0); copy_point(g, 3, 2); emit(g, header, 3); break;
	case 16: near_point(g, 3, 0, 4); near_point(g, 3, 2, 3); emit(g, header, 4);
	         copy_point(g, 3, 1); copy_point(g, 4, 2); emit(g, header, 3); break;
	case 18: near_point(g, 0, 1, 0); near_point(g, 3, 2, 3); emit(g, header, 4); break;
	case 22: near_point(g, 1, 2, 1); near_point(g, 3, 2, 0); emit(g, header, 3); break;
	case 24: near_point(g, 3, 0, 3); near_point(g, 2, 1, 2); emit(g, header, 4); break;
	case 26: near_point(g, 0, 1, 0); near_point(g, 2, 1, 2); emit(g, header, 3); break;
	case 28: near_point(g, 3, 0, 2); near_point(g, 1, 0, 1); emit(g, header, 3); break;
	default: break;   /* 10, 20, 30: nothing drawn */
	}
}

/* C0000258: the same for triangles */
static void clip_triangle(geometry_t *g, uint16_t header, int behind)
{
	switch (behind) {
	case 2:  near_point(g, 0, 2, 3); near_point(g, 0, 1, 0); emit(g, header, 4); break;
	case 4:  near_point(g, 1, 2, 4); near_point(g, 1, 0, 1); copy_point(g, 2, 3); copy_point(g, 4, 2);
	         emit(g, header, 4); break;
	case 6:  near_point(g, 1, 2, 1); near_point(g, 0, 2, 0); emit(g, header, 3); break;
	case 8:  near_point(g, 2, 0, 3); near_point(g, 2, 1, 2); emit(g, header, 4); break;
	case 10: near_point(g, 0, 1, 0); near_point(g, 2, 1, 2); emit(g, header, 3); break;
	case 12: near_point(g, 2, 0, 2); near_point(g, 1, 0, 1); emit(g, header, 3); break;
	default: break;   /* 14: nothing drawn */
	}
}

/* Clipping works on a copy of the slots (C0000092), so that the next polygon in a strip
   still finds the original vertices. */
static void clip(geometry_t *g, uint16_t header, int vertex_count, int behind)
{
	memcpy(&g->slots[5], &g->slots[0], 5 * sizeof(vertex_t));
	g->base = 5;
	if (vertex_count == 4)
		clip_quad(g, header, behind);
	else
		clip_triangle(g, header, behind);
	g->base = 0;
}

/* C00000FC */
static void load_slot(geometry_t *g, int n, uint32_t *model)
{
	uint16_t index = sh2_word(*model);
	*model += 2;
	g->slots[n] = g->vertices[(index / 16) % MAX_VERTICES];
}

/* C0000058-C00000F6: reads one polygon into the slots. Header bits 9-11 say which
   vertices it shares with the previous polygon (a strip): 0 none, 2 keeps v0 and moves
   v3 to v1, 4 moves v3 to v0 and v2 to v1, 6 moves v2 to v0. */
static void load_polygon(geometry_t *g, uint16_t header, uint32_t *model, int with_normal)
{
	int type = (header >> 8) & 0x0E;
	g->colour = sh2_word(*model);
	*model += 2;
	if (type == 0) {
		load_slot(g, 0, model);
		load_slot(g, 1, model);
	} else if (type == 2) {
		g->slots[1] = g->slots[3];
	} else if (type == 4) {
		g->slots[0] = g->slots[3];
		g->slots[1] = g->slots[2];
	} else {
		g->slots[0] = g->slots[2];
	}
	load_slot(g, 2, model);
	if (g->colour & 1)
		g->slots[3] = g->slots[2];   /* a triangle: v3 repeats v2 */
	else
		load_slot(g, 3, model);

	if (with_normal && (header & 0x20)) {
		for (int i = 0; i < 3; i++)
			g->normal[i] = (int32_t)sh2_signed_word(*model + 2 * i) << 2;
		*model += 6;
	}
}

/* Overlay A, C0000000: draws one model with the current matrix */
static void draw_model(geometry_t *g, uint32_t model)
{
	int vertex_count = sh2_word(model);
	model += 4;   /* the second word is not used here */
	transform_vertices(g, &model, vertex_count);

	for (;;) {
		if (g->record_address >= g->record_limit)
			return;
		uint16_t header = sh2_word(model);
		model += 2;
		if (((header >> 8) & 0x0E) == 0x0C)
			return;   /* end of the model */
		load_polygon(g, header, &model, 1);

		int corners = (header & 1) ? 3 : 4;
		int behind;
		if (outcode(g, corners, &behind))
			continue;
		if (behind)
			clip(g, header, corners, behind);
		else
			emit(g, header, corners);
	}
}

/* 060024DC: the records' own models with their matrices */
static void draw_records(geometry_t *g, int first, int count)
{
	for (int i = 0; i < count; i++) {
		int record = first + i * RECORD_SIZE;
		if (field(record, FLAG) == 0)
			continue;
		g->matrix = object_matrix[record_index(record)];
		g->extra = record_extra[record_index(record)];
		draw_model(g, scene_long(record + MODEL));
		g->extra = 0;
	}
}

/* 060023B4: records whose +10 points to a list of up to 4 models */
static void draw_model_lists(geometry_t *g, int first, int count)
{
	for (int i = 0; i < count; i++) {
		int record = first + i * RECORD_SIZE;
		if (field(record, FLAG) == 0)
			continue;
		g->matrix = object_matrix[record_index(record)];
		uint32_t list = scene_long(record + MODEL);
		for (int j = 0; j < 4; j++) {
			uint32_t model = (uint32_t)sh2_word(list + 4 * j) << 16 | sh2_word(list + 4 * j + 2);
			if (model != 0)
				draw_model(g, model);
		}
	}
}

/* 0600244C (master: segments 0, 2, 4, ...) and 06002494 (slave: 1, 3, 5, ...) */
static void draw_track(geometry_t *g)
{
	int count = field(CAMERA, EXTRA);
	g->matrix = object_matrix[record_index(CAMERA)];
	g->vertex_shift = 11;
	for (int i = g->half; i < count; i += 2)
		draw_model(g, scene_long(TRACK_LIST + 4 * i));
	g->vertex_shift = 6;
}

/* ---- Detailed opponents ----

   The 68000 picks each opponent's model by its distance from the player's car (a level
   in the car's state, +C1; 0036DE): near ones get the detailed model and their wheels,
   further ones (about 330 world units) a simpler model without wheels, and from about
   700 a simpler one still. The models come from a table for each kind of car (ROM
   0158E4: three 68000 addresses), with a row of four longs for each colour: 0, then
   the near, middle and far model. With detailed opponents on, every opponent gets its
   near model and wheels. These are polygons the SH2s did not draw, so they take none of
   the SH2s' room (as the far view's). */

#define CAR_MODEL_TABLES 0x0158E4
#define CAR_KINDS        3
#define CAR_COLOURS      4
#define ROM_68000        0x880000   /* the cartridge, as the 68000 sees it in 32X mode */

static int detailed_opponents;

void hle_set_detailed_opponents(int on)
{
	settings_version++;
	detailed_opponents = on;
}

/* The near model of the car whose middle or far model this is, or 0 */
static uint32_t near_model(uint32_t model)
{
	for (int kind = 0; kind < CAR_KINDS; kind++) {
		uint32_t table = rom_long(CAR_MODEL_TABLES + 4 * kind) - ROM_68000;
		for (int colour = 0; colour < CAR_COLOURS; colour++) {
			uint32_t row = table + 16 * colour;
			if (model == rom_long(row + 8) || model == rom_long(row + 12))
				return rom_long(row + 4);
		}
	}
	return 0;
}

/* Before the matrices: each opponent the 68000 gave a simpler model gets its near model,
   and its wheels (the 68000 keeps their positions and angles up to date regardless) */
static void detail_opponents(void)
{
	for (int i = 0; i < 15; i++) {
		int body = OPPONENTS + i * OPPONENT_SIZE;
		uint32_t near = 0;
		if (detailed_opponents && field(body, FLAG) != 0)
			near = near_model(scene_long(body + MODEL));
		if (near == 0)
			continue;
		set_scene_long(body + MODEL, near);
		set_scene_word(body + RECORD_SIZE + FLAG, 1);
		set_scene_word(body + 2 * RECORD_SIZE + FLAG, 1);
		for (int n = 0; n < 3; n++)
			record_extra[record_index(body) + n] = 1;
	}
}

/* The near plane for the track and the roadside, as the game sets it for its 4:3 picture
   (0x7000 in the cockpit view). Our wider picture, and the rows above and below the 3D,
   show the ground right beside the car, closer than that: the near plane cut it away and
   the Plane B behind showed through (most in the bottom corners of a picture wider than
   16:9). With the limit on (hle_set_near_limit) it is brought in to at most 0x4000; the
   checks keep the game's, as the SH2s draw. */
#define NEAR_LIMIT 0x4000
static int near_limit;

void hle_set_near_limit(int on)
{
	near_limit = on;
}

static int32_t track_near(void)
{
	int32_t near = (int16_t)scene_word(TRACK_NEAR) << 8;
	if (near_limit && near > NEAR_LIMIT)
		near = NEAR_LIMIT;
	return near;
}

/* 060022B0 / 060022BC */
static void start_geometry(geometry_t *g, int half)
{
	memset(g, 0, sizeof(*g));
	g->half = half;
	g->focal = (int16_t)scene_word(FOCAL_LENGTH);
	g->vertex_shift = 6;
	g->record_address = half == 0 ? 0x06010000 : 0x06013000;
	g->record_limit = half == 0 ? 0x06012FEC : 0x06015FEC;
	set_near(g, track_near());
}

/* 06000EB4 */
static void master_geometry(void)
{
	static geometry_t g;
	start_geometry(&g, 0);
	draw_track(&g);
	set_near(&g, 0x1000);
	draw_records(&g, PLAYER_CAR, 1);
	set_near(&g, 0x8000);
	for (int i = 0; i < 15; i += 2)
		draw_records(&g, OPPONENTS + i * OPPONENT_SIZE, 3);
	set_near(&g, track_near());
	draw_records(&g, ROADSIDE, 16);
	draw_model_lists(&g, SIGNS + 2 * RECORD_SIZE, 2);
}

/* 06000FA8 */
static void slave_geometry(void)
{
	static geometry_t g;
	start_geometry(&g, 1);
	draw_track(&g);
	set_near(&g, 0x1000);
	draw_records(&g, PLAYER_PARTS, 4);
	draw_records(&g, EFFECTS, 8);
	set_near(&g, 0x8000);
	for (int i = 1; i < 15; i += 2)
		draw_records(&g, OPPONENTS + i * OPPONENT_SIZE, 3);
	draw_model_lists(&g, SIGNS, 2);
}

/* ---- Far view ----

   Each course's track is a grid in the ROM of 32 x 32 cells, 1024 world units square:
   the long at ROM grid + 4 * (32 * row + column) is the model drawn for that square
   (GRID_EMPTY for none), with column 16 + x / 1024 and row 15 - z / 1024 (rounded down).
   The 68000 sends the cells around the player's car: 3 ahead, 1 behind and 2 to each
   side. With the SH2s' own limit (z 255, about 4000 units), the track ends 3000 to 4000
   units ahead. Far view adds the cells in sight up to far_view_limit (by default twice as
   far), drawn after everything else and without taking the SH2s' room. */

#define GRID_SIZE  32
#define GRID_EMPTY 0x2207FFFE
#define CELL_SIZE  (1024.0 / 16)   /* world units, in FIXED_ONE units (x and z are 1/16) */

/* The ROM offsets of the five courses' grids */
static const uint32_t course_grids[] = { 0x15C000, 0x15D000, 0x15E000, 0x15F000, 0x161000 };

static uint32_t grid_cell(uint32_t grid, int cell)
{
	return rom_long(grid + 4 * cell);
}

/* The grid holding this model, or 0 */
static uint32_t grid_of(uint32_t model)
{
	static uint32_t last_model, last_grid;
	if (model == last_model)
		return last_grid;
	last_model = model;
	last_grid = 0;
	for (size_t i = 0; i < sizeof(course_grids) / sizeof(course_grids[0]); i++)
		for (int cell = 0; cell < GRID_SIZE * GRID_SIZE; cell++)
			if (grid_cell(course_grids[i], cell) == model)
				return last_grid = course_grids[i];
	return 0;
}

static int sent_by_68000(uint32_t model)
{
	int count = field(CAMERA, EXTRA);
	for (int i = 0; i < count; i++)
		if (scene_long(TRACK_LIST + 4 * i) == model)
			return 1;
	return 0;
}

/* Which cells the far view adds: those reached from the player's car's cell in at most
   far_view_tiles steps, stepping only onto cells with a model (to the 8 neighbours: 1 step
   straight, 1.41 diagonally), whichever way the camera looks. It stands for the distance
   along the course, which the grid does not record (the 68000 picks its own cells by
   position: 3 ahead, 1 behind and 2 to each side): the MD version's far view adds sections
   by the course's order, so that what it draws does not change as the view turns, and
   other legs of the course seen across a gap (Bay Bridge's way back, across the water)
   are left out. A window turning with the camera made cells pop in and out at its sides. */
#define FAR_AWAY 1e9f

static float tile_steps[GRID_SIZE * GRID_SIZE];   /* from the car's cell; FAR_AWAY if not reached */
static uint32_t steps_grid;                         /* the grid and cell they are from */
static int steps_from = -1, steps_tiles = -1;

static int car_cell(void)
{
	int column = 16 + (field(PLAYER_CAR, POS_X) >> 10), row = 15 - (field(PLAYER_CAR, POS_Z) >> 10);
	if (column < 0 || column >= GRID_SIZE || row < 0 || row >= GRID_SIZE)
		return -1;
	return row * GRID_SIZE + column;
}

/* Dijkstra over the grid's cells, up to far_view_tiles; again only when the car changes
   cell (or the course or the setting changes) */
static void walk_tiles(uint32_t grid)
{
	int start = car_cell();
	if (grid == steps_grid && start == steps_from && far_view_tiles == steps_tiles)
		return;
	steps_grid = grid;
	steps_from = start;
	steps_tiles = far_view_tiles;
	static uint8_t done[GRID_SIZE * GRID_SIZE];
	for (int i = 0; i < GRID_SIZE * GRID_SIZE; i++)
		tile_steps[i] = FAR_AWAY;
	memset(done, 0, sizeof(done));
	if (start < 0)
		return;
	tile_steps[start] = 0;
	for (;;) {
		int nearest = -1;
		for (int i = 0; i < GRID_SIZE * GRID_SIZE; i++) {
			if (!done[i] && tile_steps[i] < FAR_AWAY && (nearest < 0 || tile_steps[i] < tile_steps[nearest]))
				nearest = i;
		}
		if (nearest < 0 || tile_steps[nearest] > far_view_tiles)
			return;
		done[nearest] = 1;
		int row = nearest / GRID_SIZE, column = nearest % GRID_SIZE;
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				int r = row + dy, c = column + dx, next = r * GRID_SIZE + c;
				if ((dx == 0 && dy == 0) || r < 0 || r >= GRID_SIZE || c < 0 || c >= GRID_SIZE
					|| grid_cell(grid, next) == GRID_EMPTY)
					continue;
				float steps = tile_steps[nearest] + (dx && dy ? (float)M_SQRT2 : 1.0f);
				if (steps < tile_steps[next])
					tile_steps[next] = steps;
			}
		}
	}
}

static int cell_reached(int cell)
{
	return tile_steps[cell] <= far_view_tiles;
}

/* A record's position (a prop): its cell, or for a prop on an empty cell, a neighbour */
static int in_far_window(int record)
{
	int column = 16 + (field(record, POS_X) >> 10), row = 15 - (field(record, POS_Z) >> 10);
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			int r = row + dy, c = column + dx;
			if (r >= 0 && r < GRID_SIZE && c >= 0 && c < GRID_SIZE && cell_reached(r * GRID_SIZE + c))
				return 1;
		}
	}
	return 0;
}

/* Whether some of the cell may be in sight: not wholly behind the camera, beyond the far
   view or outside the sides (side_reach: twice the distance for 16:9). Its corners are
   taken at the car's height; a margin of a cell allows for hills and what stands on the
   cell's edge. */
static int cell_in_sight(const matrix_t *view, int cell, double ground)
{
	int row = cell / GRID_SIZE, column = cell % GRID_SIZE;
	double margin = CELL_SIZE, far = far_view_limit / FIXED_ONE;
	int behind = 0, beyond = 0, left = 0, right = 0;
	for (int corner = 0; corner < 4; corner++) {
		double world[3] = { (column - 16 + (corner & 1)) * CELL_SIZE, ground,
			(15 - row + (corner >> 1)) * CELL_SIZE };
		double x = exact_dot3(view->f[0], world) + view->f[0][3];
		double z = exact_dot3(view->f[2], world) + view->f[2][3];
		behind += z < -margin;
		beyond += z > far + margin;
		double reach = side_reach / 256.0 * z;
		left += x < -reach - margin;
		right += x > reach + margin;
	}
	return behind < 4 && beyond < 4 && left < 4 && right < 4;
}

static void far_track(void)
{
	int count = field(CAMERA, EXTRA);
	uint32_t grid = count > 0 ? grid_of(scene_long(TRACK_LIST)) : 0;
	if (grid == 0)
		return;
	static geometry_t g;
	start_geometry(&g, 1);
	g.extra = 1;
	g.matrix = object_matrix[record_index(CAMERA)];
	g.vertex_shift = 11;
	double ground = exact_position(PLAYER_CAR, POS_Y, 8);
	for (int cell = 0; cell < GRID_SIZE * GRID_SIZE; cell++) {
		uint32_t model = grid_cell(grid, cell);
		if (model == GRID_EMPTY || sent_by_68000(model) || !cell_reached(cell)
			|| !cell_in_sight(&g.matrix, cell, ground))
			continue;
		draw_model(&g, model);
	}
}

/* The course being raced (the index of its grid), or -1 */
static int course_index(void)
{
	uint32_t grid = field(CAMERA, EXTRA) > 0 ? grid_of(scene_long(TRACK_LIST)) : 0;
	for (int i = 0; i < (int)(sizeof(course_grids) / sizeof(course_grids[0])); i++)
		if (course_grids[i] == grid)
			return i;
	return -1;
}

/* ---- Props in the far view ----

   The props by the track (the start gate, checkpoint banners, the fair's Ferris wheel
   and pirate ship, ...) are roadside records the 68000 writes each tick (0037B6), only
   while the player's car is within 3200 world units of them in x and z (a box; some
   also limit y):
   - a list of 3 for each course (ROM 015A64: a 68000 address for each course, the
     course number being the grid's): the start gate and two checkpoints, entries of
     14 bytes: x, y, z, angle y, x scale, and a list of 6 animation frames' models.
     Only the first within reach is written, to the record at +59C;
   - each course's own (0038xx: a routine for each course): see special_props.
   With the SH2s' own limit (z 200, 3200 units) too, they appeared 2500 to 3200 units
   ahead. Far view writes the ones the 68000 did not, as it would, and draws them up to
   the far view's end (in_far_view): each course's own to their usual records (keeping
   the animation angle the 68000 writes there every tick regardless), the lists' to
   records left unused. */

#define PROP_LISTS      0x015A64
#define PROP_LIST_SIZE  3
#define PROP_ENTRY_SIZE 14
#define PROP_FRAMES     6
#define GENERIC_PROP    0x59C   /* the record the 68000 writes the list's to */
#define ROADSIDE_END    (ROADSIDE + 16 * RECORD_SIZE)

/* Writes a scene word, exactly as well */
static void put_word(int record, int offset, int16_t value)
{
	set_scene_word(record + offset, (uint16_t)value);
	exact_word[(record + offset) / 2] = value;
}

static int16_t rom_word(uint32_t offset)
{
	return (int16_t)sh2_word(ROM_START + offset);
}

/* A record as the 68000 writes it from its data: x, y, z, a word to `angle`, the x scale
   (EXTRA), and the model; drawn without taking the SH2s' room */
static void put_prop(int record, int flag, uint32_t data, int angle, uint32_t model)
{
	put_word(record, FLAG, flag);
	put_word(record, POS_X, rom_word(data));
	put_word(record, POS_Y, rom_word(data + 2));
	put_word(record, POS_Z, rom_word(data + 4));
	put_word(record, angle, rom_word(data + 6));
	put_word(record, EXTRA, rom_word(data + 8));
	set_scene_long(record + MODEL, model);
	record_extra[record_index(record)] = 1;
}

/* A prop with a moving part (data: two records' worth of 14 bytes each, the part's
   angle z written by the 68000 regardless) */
static void put_prop_with_part(int record, uint32_t data)
{
	put_prop(record, 2, data, ANGLE_Y, rom_long(data + 10));
	put_prop(record + RECORD_SIZE, 2, data + 14, ANGLE_Y, rom_long(data + 24));
}

/* Each course's own props: the 68000's routines (jump table at 00383E, by course) */
static void special_props(int course)
{
	switch (course) {
	case 0:   /* 00385E: the Ferris wheel and the pirate ship, turning and swinging */
		if (field(0x5B0, FLAG) == 0)
			put_prop_with_part(0x5B0, 0x3924);
		if (field(0x5D8, FLAG) == 0)
			put_prop_with_part(0x5D8, 0x3940);
		break;
	case 1:   /* 00397C: a turning prop and, with it, a still one (model at 0039E4) */
		if (field(0x5D8, FLAG) == 0) {
			put_prop_with_part(0x5D8, 0x3A80);
			put_prop(0x5C4, 1, 0x3A76, ANGLE_Y, 0x222A218E);
		}
		break;
	case 3:   /* 003AAA: a prop turning about y (angle y written regardless) */
		if (field(0x5B0, FLAG) == 0)
			put_prop(0x5B0, 1, 0x3A9C, ANGLE_Z, rom_long(0x3A9C + 10));
		break;
	}
}

/* Whether a roadside record holds a prop at this position */
static int prop_sent(int16_t x, int16_t z)
{
	for (int record = ROADSIDE; record < ROADSIDE_END; record += RECORD_SIZE)
		if (field(record, FLAG) != 0 && field(record, POS_X) == x && field(record, POS_Z) == z)
			return 1;
	return 0;
}

/* The next record no object uses, from `record` on (as roadside_matrices walks them), or
   ROADSIDE_END */
static int free_record(int record)
{
	int walk = ROADSIDE;
	while (walk < ROADSIDE_END) {
		int flag = field(walk, FLAG);
		if (flag == 0 && walk >= record)
			return walk;
		walk += flag == 2 ? 2 * RECORD_SIZE : RECORD_SIZE;
	}
	return ROADSIDE_END;
}

/* The animation frame the list's props show: the 68000's own if it wrote one (it moves
   on every second tick), or else by the scenes seen */
static int scenes_taken;

static int prop_frame(uint32_t list)
{
	uint32_t model = scene_long(GENERIC_PROP + MODEL);
	if (field(GENERIC_PROP, FLAG) != 0)
		for (int k = 0; k < PROP_LIST_SIZE; k++) {
			uint32_t frames = rom_long(list + PROP_ENTRY_SIZE * k + 10) - ROM_68000;
			for (int frame = 0; frame < PROP_FRAMES; frame++)
				if (rom_long(frames + 4 * frame) == model)
					return frame;
		}
	return scenes_taken / 2 % PROP_FRAMES;
}

/* Puts a list's props the 68000 did not write in free records. Entries of `size`
   bytes; `model` 0 means each entry ends with its list of animation frames. */
static void list_props(uint32_t list, int count, int size, uint32_t model, int frame)
{
	int record = ROADSIDE;
	for (int k = 0; k < count; k++) {
		uint32_t entry = list + size * k;
		if (prop_sent(rom_word(entry), rom_word(entry + 4)))
			continue;
		record = free_record(record);
		if (record == ROADSIDE_END)
			return;
		uint32_t frames = model == 0 ? rom_long(entry + 10) - ROM_68000 : 0;
		put_prop(record, 1, entry, ANGLE_Y, model != 0 ? model : rom_long(frames + 4 * frame));
	}
}

static void far_props(void)
{
	int course = course_index();
	if (course < 0)
		return;
	special_props(course);
	uint32_t list = rom_long(PROP_LISTS + 4 * course) - ROM_68000;
	list_props(list, PROP_LIST_SIZE, PROP_ENTRY_SIZE, 0, prop_frame(list));
	if (course == 1)   /* 00395C: four alike, the first within reach at +5B0 (model at 00396A) */
		list_props(0x3A4E, 4, 10, 0x22295A24, 0);
}

/* ---- HUD polygons (master, 060010AC) ----

   Flat 2D-like models: no culling, clipping or depth; drawn after everything else in
   the order they are made. */

static hle_polygon_t hud[256];
static int hud_count;

/* 06003034: the polygon loop, with slots holding only screen positions */
static void hud_polygons(geometry_t *g, uint32_t model)
{
	for (;;) {
		uint16_t header = sh2_word(model);
		model += 2;
		if (((header >> 8) & 0x0E) == 0x0C)
			return;
		load_polygon(g, header, &model, 0);

		if (hud_count == (int)(sizeof(hud) / sizeof(hud[0])))
			continue;
		hle_polygon_t *polygon = &hud[hud_count++];
		polygon->colour = g->colour;
		polygon->vertex_count = (header & 1) ? 3 : 4;
		for (int n = 0; n < polygon->vertex_count; n++) {
			polygon->x[n] = g->slots[n].sx;
			polygon->y[n] = g->slots[n].sy;
			polygon->fx[n] = g->slots[n].fsx;
			polygon->fy[n] = g->slots[n].fsy;
			polygon->depth[n] = 0;
		}
	}
}

/* 060030E8: perspective projection, then an offset (+10: x in the high word, y low) */
static void hud_model_perspective(geometry_t *g, uint32_t model, uint32_t offset)
{
	int count = sh2_word(model + 2);   /* here the second word is the vertex count */
	model += 4;
	for (int i = 0; i < count && i < MAX_VERTICES; i++, model += 6) {
		int32_t v[3];
		for (int j = 0; j < 3; j++)
			v[j] = (int32_t)sh2_signed_word(model + 2 * j) << g->vertex_shift;
		int32_t z = dot3(g->matrix.m[2], v) + g->matrix.m[2][3];
		int32_t x = dot3(g->matrix.m[0], v) + g->matrix.m[0][3];
		int32_t y = dot3(g->matrix.m[1], v) + g->matrix.m[1][3];
		int32_t scale = divide((int64_t)g->focal << 32, z);
		vertex_t *out = &g->vertices[i];
		out->sx = (int16_t)(multiply_high(scale, x) + (int16_t)(offset >> 16));
		out->sy = (int16_t)(multiply_high(scale, y) + (int16_t)offset);
		transform_exact(&g->matrix, v, out);
		if (out->fz > 0) {
			out->fsx = (float)(g->focal * out->fx / out->fz + (int16_t)(offset >> 16));
			out->fsy = (float)(g->focal * out->fy / out->fz + (int16_t)offset);
		} else {
			out->fsx = out->sx;
			out->fsy = out->sy;
		}
	}
	hud_polygons(g, model);
}

/* 06002F94 / 06003178: one scale for the whole model, from the matrix's z */
static void hud_model_flat(geometry_t *g, uint32_t model, uint32_t offset)
{
	int32_t scale = divide((int64_t)g->focal << 32, g->matrix.m[2][3]);   /* 06002FCC */
	double exact_scale = g->focal / g->matrix.f[2][3];
	int count = sh2_word(model + 2);
	model += 4;
	for (int i = 0; i < count && i < MAX_VERTICES; i++, model += 6) {
		int32_t v[3];
		for (int j = 0; j < 3; j++)
			v[j] = (int32_t)sh2_signed_word(model + 2 * j) << g->vertex_shift;
		int32_t x = dot3(g->matrix.m[0], v) + g->matrix.m[0][3];
		int32_t y = dot3(g->matrix.m[1], v) + g->matrix.m[1][3];
		vertex_t *out = &g->vertices[i];
		out->sx = (int16_t)(multiply_high(scale, x) + (int16_t)(offset >> 16));
		out->sy = (int16_t)(multiply_high(scale, y) + (int16_t)offset);
		transform_exact(&g->matrix, v, out);
		out->fsx = (float)(exact_scale * out->fx + (int16_t)(offset >> 16));
		out->fsy = (float)(exact_scale * out->fy + (int16_t)offset);
	}
	hud_polygons(g, model);
}

static uint32_t screen_offset(int record)
{
	return (uint32_t)scene_word(record + POS_X) << 16 | scene_word(record + POS_Y);
}

/* 06003220: a square on the course map for each opponent, in its body's +0E colour */
static void course_map_dots(geometry_t *g)
{
	int32_t scale = divide((int64_t)g->focal << 32, g->matrix.m[2][3]);
	double exact_scale = g->focal / g->matrix.f[2][3];
	uint32_t offset = screen_offset(COURSE_MAP);
	for (int i = 0; i < 15; i++) {
		int body = OPPONENTS + i * OPPONENT_SIZE;
		uint16_t colour = scene_word(body + EXTRA);
		if (colour == 0)
			continue;
		int32_t x = (field(body, POS_X) << 12) - 0x100000;
		int32_t z = (field(body, POS_Z) << 12) + 0x100000;
		int32_t corners[4][3] = {
			{ x, 0, z }, { x + 0x200000, 0, z }, { x + 0x200000, 0, z - 0x200000 }, { x, 0, z - 0x200000 },
		};
		double fx = exact_position(body, POS_X, 12) - 16, fz = exact_position(body, POS_Z, 12) + 16;
		double exact_corners[4][3] = {
			{ fx, 0, fz }, { fx + 32, 0, fz }, { fx + 32, 0, fz - 32 }, { fx, 0, fz - 32 },
		};
		if (hud_count == (int)(sizeof(hud) / sizeof(hud[0])))
			return;
		hle_polygon_t *polygon = &hud[hud_count++];
		polygon->colour = colour;
		polygon->vertex_count = 4;
		for (int n = 0; n < 4; n++) {
			int32_t px = dot3(g->matrix.m[0], corners[n]) + g->matrix.m[0][3];
			int32_t py = dot3(g->matrix.m[1], corners[n]) + g->matrix.m[1][3];
			polygon->x[n] = (int16_t)(multiply_high(scale, px) + (int16_t)(offset >> 16));
			polygon->y[n] = (int16_t)(multiply_high(scale, py) + (int16_t)offset);
			vertex_t exact;
			transform_exact_vector(&g->matrix, exact_corners[n], &exact);
			polygon->fx[n] = (float)(exact_scale * exact.fx + (int16_t)(offset >> 16));
			polygon->fy[n] = (float)(exact_scale * exact.fy + (int16_t)offset);
		}
	}
}

/* 060010AC */
/* The HUD polygons made since *first belong to a model placed at this x; *first moves
   past them */
static void mark_hud_anchor(int *first, int anchor)
{
	for (; *first < hud_count; (*first)++)
		hud[*first].hud_anchor = anchor;
}

static void build_hud(void)
{
	static geometry_t g;
	memset(&g, 0, sizeof(g));
	g.focal = 0x70;
	g.vertex_shift = 6;

	int first = hud_count;
	if (field(HUD_MODEL, FLAG) != 0) {
		g.matrix = object_matrix[record_index(HUD_MODEL)];
		hud_model_perspective(&g, scene_long(HUD_MODEL + MODEL), 0);
	}
	mark_hud_anchor(&first, 0);
	if (field(NEEDLE, FLAG) != 0) {
		g.matrix = object_matrix[record_index(NEEDLE)];
		hud_model_flat(&g, scene_long(NEEDLE + MODEL), screen_offset(NEEDLE));
	}
	mark_hud_anchor(&first, field(NEEDLE, POS_X));
	if (field(COURSE_MAP, FLAG) != 0) {
		g.focal = 1;
		g.vertex_shift = 11;
		g.matrix = object_matrix[record_index(COURSE_MAP)];
		hud_model_flat(&g, scene_long(COURSE_MAP + MODEL), screen_offset(COURSE_MAP));
		course_map_dots(&g);
	}
	mark_hud_anchor(&first, field(COURSE_MAP, POS_X));
}

/* ---- Putting it together ---- */

#define DEMO_VIEW_TOP    0x0C8   /* scene words: the demo's 3D view lines */
#define DEMO_VIEW_BOTTOM 0x0CA

/* exact_word from the scene's words as they are */
static void exact_from_scene(void)
{
	for (int i = 0; i < HLE_SCENE_SIZE / 2; i++)
		exact_word[i] = (int16_t)scene_word(2 * i);
}

/* The polygon list from `scene` and exact_word */
static int build(int command, hle_frame_t *frame)
{
	frame->count = 0;
	frame->camera_t = 1;
	frame->command = command;
	frame->scene = scenes_taken;
	frame->view_top = 16;          /* overlay B's clipping window (060032D4) */
	frame->view_bottom = 207;
	if (command == HLE_DEMO_COMMAND) {
		/* 06001924: the demo takes the window from the scene, and draws no 3D when the
		   top is -1 or both are equal */
		frame->view_top = (int16_t)scene_word(DEMO_VIEW_TOP);
		frame->view_bottom = (int16_t)scene_word(DEMO_VIEW_BOTTOM);
		if (frame->view_top == -1 || frame->view_top == frame->view_bottom)
			return 0;
	}
	memset(&buckets, 0, sizeof(buckets));
	pool_count = 0;

	memset(record_extra, 0, sizeof(record_extra));
	detail_opponents();
	if (far_view) {
		int count = field(CAMERA, EXTRA);
		walk_tiles(count > 0 ? grid_of(scene_long(TRACK_LIST)) : 0);
		far_props();
	}
	build_matrices();
	sort_track_segments();
	master_geometry();
	slave_geometry();
	if (far_view)
		far_track();
	hud_count = 0;
	if (command == HLE_RACE_COMMAND)
		build_hud();   /* the demo has none (06001924 does not call 060010AC) */

	/* Overlay B (060011B4, 06004334): buckets from far to near; in each, the slave's
	   polygons first, then the master's, in the order they were put in. */
	for (int bucket = ALL_BUCKETS - 1; bucket >= 0; bucket--)
		for (int half = 1; half >= 0; half--)
			for (int i = 0; i < buckets.entries[bucket][half]; i++)
				frame->polygons[frame->count++] = pool[buckets.entry[bucket][half][i]];
	frame->hud_first = frame->count;
	for (int i = 0; i < hud_count && frame->count < HLE_MAX_POLYGONS; i++)
		frame->polygons[frame->count++] = hud[i];
	return 1;
}

int hle_build(const uint8_t *scene_in, int command, hle_frame_t *frame)
{
	memcpy(scene, scene_in, HLE_SCENE_SIZE);
	exact_from_scene();
	return build(command, frame);
}

/* The polygon lists built from the scenes, as the game goes (see "Following the game"
   below) */
static hle_frame_t frames[3];
static int latest = -1;      /* built from the last scene sent */
static int finished = -1;    /* the one the SH2s have finished drawing */
static int shown = -1;       /* the one in the frame buffer on screen */

/* ---- Between ticks: smooth motion (docs/game-loop.zh-TW.md) ----

   The game moves everything once a tick (3 frames) and sends the whole scene at once, so
   each scene is a snapshot of one tick. Between the last two, the positions and angles of
   the objects that are in both are interpolated, and the list is built again from that:
   the integer path from the values rounded, the exact path (drawing) from the fractions.
   Everything else (models, track segments, flags) is the newer scene's. Measured over
   the race script and the attract demo:
   - a position word of the same object moves at most 544 a tick (the player's car;
     opponents 88). More than SAME_OBJECT_MOST means a record now holds another object.
   - the camera's angles move at most 521 a tick, its position (an offset from the car)
     at most 1348, when the view changes (the game slides the camera over some ticks).
     The demo's camera cuts jump an angle by 833 or more, or the position by 1904 or
     more: the camera is not interpolated across those. */

#define SAME_OBJECT_MOST    1024
#define CAMERA_CUT_ANGLE    700
#define CAMERA_CUT_POSITION 1600
#define LONGEST_TICK        8        /* frames; scenes further apart are not consecutive ticks */
#define FULL_TURN           0x2000
#define LAST_MOVING_RECORD  HUD_MODEL

typedef struct {
	uint8_t scene[HLE_SCENE_SIZE];
	int command;
	int frame;          /* the frame it was sent in */
} snapshot_t;

static snapshot_t snapshots[2];   /* [0] the older, [1] the newer */
static int snapshot_count;
static int frame_count;           /* frames finished so far */
static hle_frame_t smooth_frame;

static int16_t snapshot_word(const snapshot_t *snapshot, int offset)
{
	return (int16_t)(snapshot->scene[offset] << 8 | snapshot->scene[offset + 1]);
}

/* How far a word moves from a to b: a 16-bit difference for positions, the shorter way
   round for angles */
static int movement(const snapshot_t *a, const snapshot_t *b, int offset)
{
	int delta = (int16_t)(snapshot_word(b, offset) - snapshot_word(a, offset));
	if ((offset - RECORDS_START) % RECORD_SIZE >= ANGLE_X)
		delta = ((delta % FULL_TURN) + FULL_TURN + FULL_TURN / 2) % FULL_TURN - FULL_TURN / 2;
	return delta;
}

static int largest_movement(const snapshot_t *a, const snapshot_t *b, int record, int first, int last)
{
	int largest = 0;
	for (int offset = first; offset <= last; offset += 2) {
		int delta = abs(movement(a, b, record + offset));
		if (delta > largest)
			largest = delta;
	}
	return largest;
}

/* Whether a record holds the same object in both scenes, moving as one. A flag of 0 is
   nothing drawn, except on the camera and the player's car: with the high camera in a
   tunnel the game sets both to 0 (the car not drawn) and still places the camera from the
   car (camera_position), so 0 and 1 are the same there. A flag of 2 means another kind of
   camera (world coordinates) or the cockpit view, so a change to or from it is a cut. */
static int moves_smoothly(const snapshot_t *a, const snapshot_t *b, int record)
{
	int flag = snapshot_word(b, record + FLAG), flag_before = snapshot_word(a, record + FLAG);
	if (record == CAMERA || record == PLAYER_CAR) {
		if ((flag == 2) != (flag_before == 2))
			return 0;
	} else if (flag == 0 || flag != flag_before) {
		return 0;
	}
	int position = largest_movement(a, b, record, POS_X, POS_Z);
	if (record != CAMERA)
		return position <= SAME_OBJECT_MOST;
	int angle = largest_movement(a, b, record, ANGLE_X, ANGLE_Z);
	return position <= CAMERA_CUT_POSITION && angle <= CAMERA_CUT_ANGLE;
}

/* `scene` and exact_word at t between a (0) and b (1) */
static void interpolate(const snapshot_t *a, const snapshot_t *b, double t)
{
	memcpy(scene, b->scene, HLE_SCENE_SIZE);
	exact_from_scene();
	for (int record = CAMERA; record <= LAST_MOVING_RECORD; record += RECORD_SIZE) {
		if (!moves_smoothly(a, b, record))
			continue;
		for (int offset = POS_X; offset <= ANGLE_Z; offset += 2) {
			/* from b backwards, so that t = 1 is b exactly */
			double exact = snapshot_word(b, record + offset) - movement(a, b, record + offset) * (1 - t);
			exact_word[(record + offset) / 2] = exact;
			set_scene_word(record + offset, (uint16_t)(int32_t)lround(exact));
		}
	}
}

int hle_scene_count(void)
{
	return scenes_taken;
}

static void take_snapshot(const uint8_t *new_scene, int command)
{
	scenes_taken++;
	snapshots[0] = snapshots[1];
	memcpy(snapshots[1].scene, new_scene, HLE_SCENE_SIZE);
	snapshots[1].command = command;
	snapshots[1].frame = frame_count;
	if (snapshot_count < 2)
		snapshot_count++;
}

/* What smooth_frame was last built from: it is built again only when one of these changes.
   Paused, or once the race is left, no scene comes and t stays at 1; building the old
   scene again then would read models from SH2 SDRAM that the game may have overwritten
   (leaving a race: the result screen's data, read as garbage polygons). */
static struct {
	int valid, scenes, settings, drawn;
	double t;
} smooth_built;

/* The scene sent in frame F is reached at the end of frame F + L (L = the tick's length,
   from the scene before): the same time the 32X starts showing the SH2s' picture of it.
   The pair before ends at t = 1 where this one starts at t = 0. With frames_per_image,
   the moment only takes steps that far apart. A step can fall a little before this pair
   starts (30 a second: every other tick starts between two steps); the image of the pair
   before then stays. */
const hle_frame_t *hle_interpolated(double fraction, const hle_frame_t *on_screen, double frames_per_image)
{
	if (snapshot_count == 0)
		return NULL;
	const snapshot_t *a = &snapshots[0], *b = &snapshots[1];
	double t = 1;
	int tick = b->frame - a->frame;
	if (snapshot_count == 2 && a->command == b->command && tick >= 1 && tick <= LONGEST_TICK) {
		double moment = frame_count - 1 + fraction;
		if (frames_per_image > 0) {
			moment = floor(moment / frames_per_image + 1e-9) * frames_per_image;
			if (moment < b->frame && smooth_built.valid && smooth_built.drawn)
				return &smooth_frame;   /* the step before this pair: the last image stays */
		}
		t = (moment - b->frame) / tick;
		t = t < 0 ? 0 : t > 1 ? 1 : t;
	}
	int unchanged = smooth_built.valid && smooth_built.scenes == scenes_taken && smooth_built.t == t
		&& smooth_built.settings == settings_version;
	if (!unchanged) {
		interpolate(a, b, t);
		smooth_built.drawn = build(b->command, &smooth_frame);
		smooth_frame.camera_t = snapshot_count == 2 && moves_smoothly(a, b, CAMERA) ? t : 1;
		smooth_built.valid = 1;
		smooth_built.scenes = scenes_taken;
		smooth_built.t = t;
		smooth_built.settings = settings_version;
	}
	if (!smooth_built.drawn)
		return NULL;
	/* The 3D's lines as on screen: the frame buffer around the 3D (the title's panels
	   closing over the demo) belongs to the picture shown, a tick behind the newest scene */
	if (on_screen != NULL) {
		smooth_frame.view_top = on_screen->view_top;
		smooth_frame.view_bottom = on_screen->view_bottom;
	}
	return &smooth_frame;
}

/* ---- Following the game: which polygon list is on screen ----

   Each tick the 68000 sends command 2 or 5 (COMM0 low byte) and the scene (DREQ to
   0600C000). The master reports the picture finished by writing 1 to COMM1's low byte;
   the 68000 then sends the next scene and, at the end of that frame, the 32X shows the
   finished frame buffer. */

#define WRITER_68K    0
#define WRITER_MASTER 1

static int command;
static int shown_buffer = -1;

static int free_frame(void)
{
	for (int i = 0; i < 3; i++)
		if (i != finished && i != shown)
			return i;
	return 0;
}

void hle_on_comm_write(int writer, unsigned int offset, unsigned int value, int size)
{
	unsigned int low_byte = size == 2 ? (value & 0xFF) : value;
	int is_low_byte_write = size == 2 || (offset & 1);
	if (!is_low_byte_write)
		return;
	if (writer == WRITER_68K && (offset & ~1u) == 0x20) {
		command = low_byte;
		if (command != HLE_RACE_COMMAND && command != HLE_DEMO_COMMAND)
			latest = -1;
	}
	if (writer == WRITER_MASTER && (offset & ~1u) == 0x22 && low_byte == 1)
		finished = latest;
}

void hle_on_dreq_write(unsigned int address)
{
	int is_last_scene_word = (address & ~CACHE_THROUGH) == HLE_SCENE_ADDRESS + HLE_SCENE_SIZE - 2;
	if (!is_last_scene_word || (command != HLE_RACE_COMMAND && command != HLE_DEMO_COMMAND))
		return;
	uint8_t new_scene[HLE_SCENE_SIZE];
	hle_read_scene(new_scene);
	take_snapshot(new_scene, command);
	latest = free_frame();
	if (!hle_build(new_scene, command, &frames[latest]))
		latest = -1;
}

const hle_frame_t *hle_on_frame_end(void)
{
	frame_count++;
	if (Pico32xMem == NULL)
		return NULL;
	int buffer = Pico32x.vdp_regs[0x0a / 2] & P32XV_FS;
	if (buffer == shown_buffer)
		return NULL;
	shown_buffer = buffer;
	shown = finished;
	return shown >= 0 ? &frames[shown] : NULL;
}

void hle_reset(void)
{
	latest = finished = shown = -1;
	snapshot_count = 0;
	smooth_built.valid = 0;
	shown_buffer = -1;
}

const hle_frame_t *hle_shown(void)
{
	return shown >= 0 ? &frames[shown] : NULL;
}

const hle_frame_t *hle_latest(void)
{
	return latest >= 0 ? &frames[latest] : NULL;
}
