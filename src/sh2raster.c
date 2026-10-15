/* The SH2s' polygon fill (see sh2raster.h). A port of tools/polyrender.py, which was
   checked pixel for pixel against the SH2s' frame buffers. */
#include <stdlib.h>

#include "sh2raster.h"

/* Overlay B's clipping window. The top and bottom are the polygon list's (16 and 207 in
   a race), set by sh2_draw_polygon. */
#define VIEW_LEFT   0
#define VIEW_RIGHT  319
static int view_top_line = 16, view_bottom_line = 207;

#define SCREEN_CENTRE_X 160
#define SCREEN_CENTRE_Y 112

typedef struct {
	int x, y;
} point_t;

/* A vertex chain from the top vertex down to the bottom one. Clipping can add a few
   points: up to 4 vertices, 2 per crossing, and 2 joining points. */
#define CHAIN_ROOM 16
typedef struct {
	point_t point[CHAIN_ROOM];
	int count;
} chain_t;

static int16_t to_signed16(int value)
{
	return (int16_t)value;
}

static int32_t to_signed32(int64_t value)
{
	return (int32_t)(uint32_t)value;
}

static void append(chain_t *chain, point_t point)
{
	if (chain->count < CHAIN_ROOM)
		chain->point[chain->count++] = point;
}

static void prepend(chain_t *chain, point_t point)
{
	if (chain->count == CHAIN_ROOM)
		return;
	for (int i = chain->count; i > 0; i--)
		chain->point[i] = chain->point[i - 1];
	chain->point[0] = point;
	chain->count++;
}

/* The game's 1/d table (SDRAM 060048D0): 0x4000 / d rounded toward zero, 32767 for 0 */
static int reciprocal(int d)
{
	if (d == 0)
		return 32767;
	return 0x4000 / d;
}

/* Per-line change of x along an edge, 16.16: MULS.W of the 16-bit x difference and
   1/(y difference), shifted left by 2 */
static int32_t edge_step(int x, int next_x, int y, int next_y)
{
	int32_t product = to_signed16(x - next_x) * to_signed16(reciprocal(y - next_y));
	return to_signed32((int64_t)product << 2);
}

/* Overlay B C00003F6: where the edge v1-v2 crosses target_y */
static point_t interpolate_at_y(point_t v1, point_t v2, int target_y)
{
	if (!(v2.y >= v1.y)) {
		point_t swap = v1;
		v1 = v2;
		v2 = swap;
	}
	int dx = v1.x - v2.x;
	int dy = v1.y - v2.y;                   /* <= 0 */
	int new_x;
	if (dy < -255) {
		/* Too tall for the table: the SH2's divider, rounding toward zero */
		int numerator = to_signed16(target_y - v1.y) * to_signed16(dx);
		new_x = v1.x + numerator / dy;
	} else {
		int32_t slope = to_signed32((int64_t)(to_signed16(dx) * to_signed16(reciprocal(dy))) << 2);
		int32_t offset = to_signed32((int64_t)(target_y - v1.y) << 16);
		new_x = v1.x + (int32_t)(((int64_t)slope * offset) >> 32);
	}
	point_t result = { to_signed16(new_x), target_y };
	return result;
}

/* Overlay B C0000372: the vertices from start to end, walking by step */
static void chain_unclipped(const point_t *v, int start, int end, int step, chain_t *chain)
{
	chain->count = 0;
	for (int index = start;; index = (index + step + 4) % 4) {
		append(chain, v[index]);
		if (index == end)
			return;
	}
}

/* Overlay B C0000392: like chain_unclipped, but edges crossing the top or bottom of the
   view end at a new vertex there. */
static void chain_clipped_vertically(const point_t *v, int start, int end, int step, chain_t *chain)
{
	chain->count = 0;
	int index = start;
	point_t current = v[index];
	point_t previous;
	if (current.y >= view_top_line) {
		append(chain, current);
		index = (index + step + 4) % 4;
	} else {
		for (;;) {
			previous = current;
			index = (index + step + 4) % 4;
			current = v[index];
			if (current.y >= view_top_line)
				break;
		}
		append(chain, interpolate_at_y(current, previous, view_top_line));
		/* The interpolation (C00003F6) leaves the upper vertex in the register that the
		   loop below takes as the previous one, so an edge that also crosses the bottom
		   is interpolated along the same edge. */
		current = previous;
	}
	for (;;) {
		previous = current;
		current = v[index];
		if (current.y > view_bottom_line) {
			append(chain, interpolate_at_y(current, previous, view_bottom_line));
			return;
		}
		append(chain, current);
		if (index == end)
			return;
		index = (index + step + 4) % 4;
	}
}

/* Overlay B C0000320: the first highest and first lowest vertex */
static void top_and_bottom(const point_t *v, int *top, int *bottom)
{
	*top = *bottom = 0;
	for (int i = 1; i < 4; i++) {
		if (v[i].y < v[*top].y)
			*top = i;
		else if (v[i].y > v[*bottom].y)
			*bottom = i;
	}
}

/* Overlay B's outcodes for x (C00004B6) */
#define LEFT_OF_VIEW  4
#define RIGHT_OF_VIEW 2

static int horizontal_outcode(int x)
{
	int code = 0;
	if (x < VIEW_LEFT)
		code |= LEFT_OF_VIEW;
	if (x > VIEW_RIGHT)
		code |= RIGHT_OF_VIEW;
	return code;
}

/* Overlay B C00004CC: where the edge crosses x = 0 or 319; the same interpolation as
   for y, with x and y swapped */
static point_t cross_vertical_border(point_t previous, point_t current, int code)
{
	int border_x = code == RIGHT_OF_VIEW ? VIEW_RIGHT : VIEW_LEFT;
	point_t a = { previous.y, previous.x }, b = { current.y, current.x };
	point_t crossing = interpolate_at_y(a, b, border_x);
	point_t result = { crossing.y, crossing.x };
	return result;
}

/* Overlay B C0000470: clips a chain at x = 0 and 319; returns the outcodes of its
   first and last vertex */
static void clip_chain_horizontally(chain_t *chain, int *first_code, int *last_code)
{
	chain_t clipped = { .count = 0 };
	*first_code = horizontal_outcode(chain->point[0].x);
	if (*first_code == 0)
		append(&clipped, chain->point[0]);
	int previous_code = *first_code;
	for (int i = 1; i < chain->count; i++) {
		point_t previous = chain->point[i - 1], current = chain->point[i];
		int current_code = horizontal_outcode(current.x);
		if (!(previous_code & current_code)) {
			if (previous_code)
				append(&clipped, cross_vertical_border(previous, current, previous_code));
			if (current_code)
				append(&clipped, cross_vertical_border(previous, current, current_code));
			if (current_code == 0)
				append(&clipped, current);
		}
		previous_code = current_code;
	}
	*last_code = previous_code;
	*chain = clipped;
}

/* Overlay B C00004F6: after horizontal clipping the two chains can start (or end) at
   different points on the view's border. A corner of the view, or the other chain's end
   point, is added so that both start (end) on the same line. */
static void join_clipped_chains(chain_t *left, chain_t *right, int left_code, int right_code, int y, int at_top)
{
	void (*add)(chain_t *, point_t) = at_top ? prepend : append;
	if (left_code == right_code) {
		if (left_code == 0)
			return;
		/* Both outside on the same side: copy the end point of the chain that reaches
		   further (up at the top, down at the bottom) to the other chain. */
		int has_left = left->count > 0, has_right = right->count > 0;
		point_t left_end = { 0, 0 }, right_end = { 0, 0 };
		if (has_left)
			left_end = at_top ? left->point[0] : left->point[left->count - 1];
		if (has_right)
			right_end = at_top ? right->point[0] : right->point[right->count - 1];
		if (at_top) {
			if (!has_left) {
				if (has_right)
					add(left, right_end);
				return;
			}
			if (!has_right || right_end.y > left_end.y)
				add(right, left_end);
			else
				add(left, right_end);
			return;
		}
		if (has_right && has_left && right_end.y > left_end.y)
			add(left, right_end);
		else if (has_left)
			add(right, left_end);
		return;
	}
	point_t corner_left = { VIEW_LEFT, y }, corner_right = { VIEW_RIGHT, y };
	if (left_code)
		add(left, left_code == LEFT_OF_VIEW ? corner_left : corner_right);
	if (right_code)
		add(right, right_code == LEFT_OF_VIEW ? corner_left : corner_right);
}

/* Overlay B's per-polygon work: two chains from the top vertex to the bottom one, or 0
   if nothing is drawn. "Left" and "right" are only walking directions here. */
static int vertex_chains(const point_t *v, chain_t *left, chain_t *right)
{
	int min_x = v[0].x, max_x = v[0].x, min_y = v[0].y, max_y = v[0].y;
	for (int i = 1; i < 4; i++) {
		if (v[i].x < min_x) min_x = v[i].x;
		if (v[i].x > max_x) max_x = v[i].x;
		if (v[i].y < min_y) min_y = v[i].y;
		if (v[i].y > max_y) max_y = v[i].y;
	}
	int needs_horizontal_clip = min_x < VIEW_LEFT || max_x > VIEW_RIGHT;
	int needs_vertical_clip = min_y < view_top_line || max_y > view_bottom_line;
	if ((needs_vertical_clip || needs_horizontal_clip) && (!(max_y > view_top_line) || min_y > view_bottom_line))
		return 0;

	int top, bottom;
	top_and_bottom(v, &top, &bottom);
	if (v[top].y == v[bottom].y)
		return 0;
	if (!needs_vertical_clip && !needs_horizontal_clip) {
		chain_unclipped(v, top, bottom, 1, left);
		chain_unclipped(v, top, bottom, -1, right);
		return 1;
	}

	chain_clipped_vertically(v, top, bottom, 1, left);
	chain_clipped_vertically(v, top, bottom, -1, right);
	if (!needs_horizontal_clip)
		return 1;

	int left_top, left_bottom, right_top, right_bottom;
	clip_chain_horizontally(left, &left_top, &left_bottom);
	clip_chain_horizontally(right, &right_top, &right_bottom);
	join_clipped_chains(left, right, left_top, right_top, view_top_line, 1);
	join_clipped_chains(left, right, left_bottom, right_bottom, view_bottom_line, 0);
	return 1;
}

/* ---- Overlay C: walking the chains ---- */

typedef struct {
	point_t point[CHAIN_ROOM + 1];   /* the chain, then the packet's end marker (-1, -1) */
	int position;
	int32_t x, step;                 /* 16.16 */
	int lines, start_y;
} walker_t;

static void start_walker(walker_t *w, const chain_t *chain)
{
	for (int i = 0; i < chain->count; i++)
		w->point[i] = chain->point[i];
	w->point[chain->count].x = -1;
	w->point[chain->count].y = -1;
	w->position = 0;
}

/* Moves to the next edge that goes down. Returns 0 at the end of the chain. The first
   call for each chain also stops at a negative x (the end marker). */
static int next_edge(walker_t *w, int check_end_x)
{
	for (;;) {
		point_t p = w->point[w->position++];
		if (check_end_x && p.x < 0)
			return 0;
		point_t next = w->point[w->position];
		if (next.y == p.y)
			continue;             /* horizontal edge: skip */
		if (p.y - next.y > 0)
			return 0;             /* going up: the chain is over */
		w->x = p.x << 16;
		w->step = edge_step(p.x, next.x, p.y, next.y);
		w->lines = next.y - p.y;
		w->start_y = p.y;
		return 1;
	}
}

enum { EDGE, FINISH, STOP };

/* Overlay C C00000C4-C0000124, the left chain's reload. Unlike next_edge, every step
   first checks whether the following vertex is the end marker; if so the chain is over
   and one more span is drawn. */
static int next_left_edge(walker_t *w)
{
	for (;;) {
		point_t p = w->point[w->position++];
		point_t next = w->point[w->position];
		if (next.y < 0)
			return FINISH;
		if (next.y == p.y)
			continue;
		if (p.y - next.y > 0)
			return STOP;
		w->x = p.x << 16;         /* no +0.5 here */
		w->step = edge_step(p.x, next.x, p.y, next.y);
		w->lines = next.y - p.y;
		return EDGE;
	}
}

static void plot(uint8_t *image, int x, int y, int index, int dithered)
{
	if (y < view_top_line || y > view_bottom_line || x < VIEW_LEFT || x > VIEW_RIGHT)
		return;
	/* Dithered polygons write only every other pixel, alternating per line (to the frame
	   buffer's overwrite area, so the rest stays as it was). */
	if (dithered && ((x + y) & 1))
		return;
	image[y * SH2_SCREEN_WIDTH + x] = index;
}

/* Overlay C C0000188: one span, both ends included. Odd left and even right ends are
   single byte writes, the pairs between come from the 32X VDP's fill. A span with
   right < left still writes those single bytes. */
static void span(uint8_t *image, int y, int left, int right, int index, int dithered)
{
	if (left == right) {
		plot(image, left, y, index, dithered);
		return;
	}
	if (left & 1) {
		plot(image, left, y, index, dithered);
		left++;
	}
	if (!(right & 1))
		plot(image, right, y, index, dithered);
	int count = right - left + 1;
	if (count >= 2)
		for (int x = left; x < left + 2 * (count >> 1); x++)
			plot(image, x, y, index, dithered);
}

/* Overlay C C00001F4: a dithered span, written from right to left: the right end alone
   if even, then pairs, then the left end alone if odd. It covers the same pixels as
   span() unless right < left, where the pixels written lie further left. */
static void dithered_span(uint8_t *image, int y, int left, int right, int index)
{
	if (left == right) {
		plot(image, left, y, index, 1);
		return;
	}
	int end = right + 1;              /* the next write goes just left of this */
	int last = right;                 /* rightmost pixel still to write */
	if (!(right & 1)) {
		plot(image, right, y, index, 1);
		end = right;
		last = right - 1;
	}
	int count = last - left + 1;
	if (count >= 2) {
		for (int pairs = count >> 1; pairs > 0; pairs--) {
			end -= 2;
			plot(image, end, y, index, 1);
			plot(image, end + 1, y, index, 1);
		}
	}
	if (left & 1)
		plot(image, end - 1, y, index, 1);
}

static void draw_span(uint8_t *image, int y, int left, int right, int index, int dithered)
{
	if (dithered)
		dithered_span(image, y, left, right, index);
	else
		span(image, y, left, right, index, 0);
}

static void rasterize(uint8_t *image, const chain_t *first_chain, const chain_t *second_chain, uint16_t colour)
{
	walker_t first, second;
	start_walker(&first, first_chain);
	start_walker(&second, second_chain);
	if (!next_edge(&first, 1) || !next_edge(&second, 1))
		return;

	int line = first.start_y;
	first.x |= 0x8000;            /* +0.5, only for the two first edges */
	second.x |= 0x8000;
	walker_t *left = &first, *right = &second;
	int keep_order = second.x > first.x || (second.x == first.x && second.step > first.step);
	if (!keep_order) {
		left = &second;
		right = &first;
	}

	int index = colour >> 8;
	int dithered = (colour & 2) != 0;
	for (;;) {
		draw_span(image, line, left->x >> 16, right->x >> 16, index, dithered);
		line++;
		left->lines--;
		left->x = to_signed32((int64_t)left->x + left->step);
		if (left->lines == 0) {
			int result = next_left_edge(left);
			if (result == FINISH) {
				/* End of the left chain: one more span on the next line, then stop */
				right->x = to_signed32((int64_t)right->x + right->step);
				draw_span(image, line, left->x >> 16, right->x >> 16, index, dithered);
				return;
			}
			if (result == STOP)
				return;
		}
		right->lines--;
		right->x = to_signed32((int64_t)right->x + right->step);
		if (right->lines == 0 && !next_edge(right, 0))
			return;
	}
}

void sh2_draw_polygon(uint8_t *image, const hle_polygon_t *polygon, int view_top, int view_bottom)
{
	if (polygon->drawn_with == HLE_EXACT_VERTICES_ONLY)
		return;   /* not in the SH2s' list */
	view_top_line = view_top;
	view_bottom_line = view_bottom;
	/* Screen coordinates; triangles repeat their last vertex */
	point_t v[4];
	for (int i = 0; i < 4; i++) {
		int n = i < polygon->vertex_count ? i : polygon->vertex_count - 1;
		v[i].x = to_signed16(SCREEN_CENTRE_X + polygon->x[n]);
		v[i].y = to_signed16(SCREEN_CENTRE_Y - polygon->y[n]);
	}
	chain_t left, right;
	if (vertex_chains(v, &left, &right))
		rasterize(image, &left, &right, polygon->colour);
}
