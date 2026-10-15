/* vrdetrace: runs the game headless from power-on, following an input script.

   By default it prints one line per frame with a hash of the picture, the sound and
   each memory area. Two runs with the same arguments must print exactly the same
   lines; comparing them is how we check that the emulation is reproducible.

   It can also record how the 68000 talks to the two SH2s (-c) and dump the SH2 memory
   and 32X frame buffers (-d), for studying what the game hands to the 3D renderer.

   usage: vrdetrace [-n frames] [-p frame:keys]... [-w frame]... [-c frame] [-a start:end]...
                    [-m start:end]... [-s percent] [-d frame]... [-v frame]... [-h frame] rom.32x
     -n  number of frames to run (default 600)
     -p  from that frame on, hold these buttons: any of S A B C U D L R X Y Z, and M for
         Mode (S = Start, U/D/L/R = up/down/left/right). "-p 900:" releases everything.
     -w  after that frame, save the picture as frame_NNNNNN.ppm
     -c  from that frame on, log every COMM register write and every DREQ transfer
     -a  with -c, also log every SH2 data read and write in this address range (hex, end
         not included); the cache-through bit (20000000) is ignored when matching.
         Up to 8 ranges, one per -a
     -m  at the end of every frame (from the -c frame on, if given), print the 68000's work
         RAM words in this range (hex addresses in FF0000-FFFFFF or 0000-FFFF, end not
         included). Up to 8 ranges, one per -m
     -s  run both SH2s at this percent of PicoDrive's usual clock (2.4 times the 68000's),
         to see what the game does when the SH2s are late
     -d  after that frame, save sdram_NNNNNN.bin (SH2 SDRAM, as the SH2 sees it),
         cache_msh2_NNNNNN.bin / cache_ssh2_NNNNNN.bin (each SH2's 4 KB cache, which the
         game uses as fast RAM at C0000000), dram_NNNNNN_0.bin / _1.bin (both 32X frame
         buffers, raw), pal32x_NNNNNN.bin (the 32X palette, 256 BGR555 words) and
         fb32x_NNNNNN_0.ppm / _1.ppm (both frame buffers decoded with that palette)
     -v  after that frame, save vdp_NNNNNN.bin: the video state for splitting the picture
         into layers (tools/vdplayers.py). All big-endian: "VDPX", the 32 Mega Drive VDP
         registers (bytes), VRAM (64 KB), CRAM (64 words, 0000BBB0GGG0RRR0), VSRAM (64
         words), then the 16 32X VDP registers (words)
     -h  from that frame on, check the HLE (src/hle.c): each time the 32X shows a newly
         finished race picture, the HLE's polygon list for it is filled with the SH2s'
         own rules (src/sh2raster.c) and compared with the frame buffer, pixel by pixel,
         in the 3D view (rows 16-207 in a race), leaving out the HUD graphics the master SH2 copies
         in (src/hud.c) and pixels neither side drew. The first three
         pictures that differ are saved as hlediff_NNNNNN.ppm (differences in magenta)
         and hlepoly_NNNNNN.txt (the HLE's polygons: colour word, then x y per vertex)
   Files are written to the current directory.

   Output lines:
     F <frame> screen <h> audio <h> ram68k <h> vdp <h> sdram <h> framebuffer <h> regs32x <h>
         one per frame; each value is a 64-bit FNV-1a hash in hex:
         screen       the finished picture (visible rows only)
         audio        the frame's sound samples
         ram68k       68000 work RAM (64 KB at FF0000)
         vdp          Mega Drive VDP memory: VRAM, CRAM (palette) and VSRAM
         sdram        SH2 SDRAM (256 KB, shared by both SH2s)
         framebuffer  both 32X frame buffers (2 x 128 KB) and the 32X palette
         regs32x      the 32X registers as the 68000 sees them (A15100 on), incl. COMM
     With -c, before each F line, the frame's events in order:
       C <frame> <line> <writer> <pc> COMM<n> <size> <value>
           a write to COMM register n (A15120 + 2n); writer is 68k, msh2 or ssh2 (master
           or slave SH2); size is b (byte; value shown in its half, other half "..")
           or w (word)
       Q <frame> <line> <sh2> <address> <word count>
           words moved from the DREQ FIFO into SH2 memory starting at address,
           followed by lines of up to 16 words: "  <word> <word> ..."
       A <frame> <line> <sh2> <pc> <R|W><size> <address> <value>
           (-a) an SH2 data access: R or W, size 1, 2 or 4 bytes
     M <frame> <address> <word> <word> ...
           (-m) the work RAM words from that address, in hex
     S <frame> shown <0|1> mode <n>
           at the end of the frame: which 32X frame buffer is on screen, and the
           32X display mode (1 = packed pixel, 256 colours)
     With -h, after each F line that showed a new race picture:
       H <frame> polygons <n> pixels <compared> same <equal>
     and a summary line at the end. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pico/pico_int.h"
#include "core.h"
#include "hle.h"
#include "hlecheck.h"
#include "hud.h"
#include "sh2raster.h"

#define MAX_SCRIPT_ENTRIES 256

/* -p: from frame `frame` on, hold `buttons` */
struct button_change {
	int frame;
	uint16_t buttons;
};

/* The frame being run, for log lines written from inside the core's hooks. */
static int current_frame;

/* ---- Hashing ---- */

/* 64-bit FNV-1a: a simple hash; any difference in the data changes it. */
#define HASH_START 0xcbf29ce484222325ull

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size)
{
	const unsigned char *bytes = data;
	for (size_t i = 0; i < size; i++) {
		hash ^= bytes[i];
		hash *= 0x100000001b3ull;
	}
	return hash;
}

static uint64_t hash_of(const void *data, size_t size)
{
	return hash_bytes(HASH_START, data, size);
}

static uint64_t hash_visible_screen(void)
{
	const uint16_t *first_visible_row = &core_screen[core_visible_top * CORE_SCREEN_WIDTH];
	size_t visible_bytes = (size_t)core_visible_lines * CORE_SCREEN_WIDTH * sizeof(uint16_t);
	return hash_of(first_visible_row, visible_bytes);
}

static uint64_t hash_vdp_memory(void)
{
	uint64_t hash = HASH_START;
	hash = hash_bytes(hash, PicoMem.vram, sizeof(PicoMem.vram));
	hash = hash_bytes(hash, PicoMem.cram, sizeof(PicoMem.cram));
	hash = hash_bytes(hash, PicoMem.vsram, sizeof(PicoMem.vsram));
	return hash;
}

/* The 32X memory is allocated when the game first switches the 32X on; before that
   these hash to 0. */
static uint64_t hash_sdram(void)
{
	if (Pico32xMem == NULL)
		return 0;
	return hash_of(Pico32xMem->sdram, sizeof(Pico32xMem->sdram));
}

static uint64_t hash_32x_framebuffers(void)
{
	if (Pico32xMem == NULL)
		return 0;
	uint64_t hash = HASH_START;
	hash = hash_bytes(hash, Pico32xMem->dram, sizeof(Pico32xMem->dram));
	hash = hash_bytes(hash, Pico32xMem->pal, sizeof(Pico32xMem->pal));
	return hash;
}

static void print_frame_hashes(int frame)
{
	size_t audio_bytes = (size_t)core_audio_frame_count * 2 * sizeof(int16_t);

	printf("F %06d", frame);
	printf(" screen %016llx", (unsigned long long)hash_visible_screen());
	printf(" audio %016llx", (unsigned long long)hash_of(core_audio_samples, audio_bytes));
	printf(" ram68k %016llx", (unsigned long long)hash_of(PicoMem.ram, sizeof(PicoMem.ram)));
	printf(" vdp %016llx", (unsigned long long)hash_vdp_memory());
	printf(" sdram %016llx", (unsigned long long)hash_sdram());
	printf(" framebuffer %016llx", (unsigned long long)hash_32x_framebuffers());
	printf(" regs32x %016llx", (unsigned long long)hash_of(Pico32x.regs, sizeof(Pico32x.regs)));
	printf("\n");
}

/* ---- Logging 68k <-> SH2 traffic (-c) ---- */

static const char *const writer_names[] = { "68k", "msh2", "ssh2" };

/* DREQ words arrive one at a time; consecutive ones are collected into one block so
   the log shows "this many words to this address" instead of one line per word. */
#define MAX_DREQ_BLOCK_WORDS 0x10000
static struct {
	int is_slave;
	int line;                          /* scanline when the block started */
	uint32_t start_address;
	uint32_t word_count;
	uint16_t words[MAX_DREQ_BLOCK_WORDS];
} dreq_block;

static void print_dreq_block(void)
{
	if (dreq_block.word_count == 0)
		return;

	printf("Q %06d %03d %s %08x %u\n", current_frame, dreq_block.line,
		writer_names[1 + dreq_block.is_slave], dreq_block.start_address, dreq_block.word_count);

	for (uint32_t i = 0; i < dreq_block.word_count; i++) {
		int first_in_row = (i % 16 == 0);
		int last_in_row = (i % 16 == 15) || (i == dreq_block.word_count - 1);
		if (first_in_row)
			printf(" ");
		printf(" %04x", dreq_block.words[i]);
		if (last_in_row)
			printf("\n");
	}
	dreq_block.word_count = 0;
}

static int logging_traffic;   /* -c is active this frame */
static int checking_hle;      /* -h is active */

static void on_dreq_write(int is_slave, unsigned int address, unsigned int value)
{
	if (checking_hle)
		hle_on_dreq_write(address);
	if (!logging_traffic)
		return;
	uint32_t next_address = dreq_block.start_address + dreq_block.word_count * 2;
	int continues_block = dreq_block.word_count > 0
		&& is_slave == dreq_block.is_slave
		&& address == next_address
		&& dreq_block.word_count < MAX_DREQ_BLOCK_WORDS;

	if (!continues_block) {
		print_dreq_block();
		dreq_block.is_slave = is_slave;
		dreq_block.line = Pico.m.scanline;
		dreq_block.start_address = address;
	}
	dreq_block.words[dreq_block.word_count] = value;
	dreq_block.word_count++;
}

static void on_comm_write(int writer, unsigned int pc, unsigned int offset, unsigned int value, int size)
{
	if (checking_hle)
		hle_on_comm_write(writer, offset, value, size);
	if (!logging_traffic)
		return;
	print_dreq_block();   /* keep the log in time order */

	int register_number = (offset - 0x20) / 2;
	printf("C %06d %03d %-4s %08x COMM%d ", current_frame, Pico.m.scanline,
		writer_names[writer], pc, register_number);

	if (size == 2) {
		printf("w %04x\n", value);
		return;
	}
	/* A byte write: even offsets are the high byte of the register. */
	int is_high_byte = (offset % 2 == 0);
	if (is_high_byte)
		printf("b %02x..\n", value);
	else
		printf("b ..%02x\n", value);
}

/* -a: the address ranges to log, without the cache-through bit */
/* -m: 68000 work RAM ranges printed every frame (offsets in the 64 KB) */
#define MAX_RAM_RANGES 8
static struct { unsigned int start, end; } ram_ranges[MAX_RAM_RANGES];
static int ram_range_count;

static void print_ram_ranges(int frame)
{
	const uint16_t *words = (const uint16_t *)PicoMem.ram;   /* PicoDrive keeps RAM as words */
	for (int i = 0; i < ram_range_count; i++) {
		printf("M %06d %06X", frame, 0xFF0000 | ram_ranges[i].start);
		for (unsigned int a = ram_ranges[i].start; a < ram_ranges[i].end; a += 2)
			printf(" %04x", words[a >> 1]);
		printf("\n");
	}
}

#define MAX_ACCESS_RANGES 8
static struct {
	unsigned int start, end;
} access_ranges[MAX_ACCESS_RANGES];
static int access_range_count;

static int in_access_ranges(unsigned int address)
{
	for (int i = 0; i < access_range_count; i++) {
		if (address >= access_ranges[i].start && address < access_ranges[i].end)
			return 1;
	}
	return 0;
}

static void on_sh2_access(int is_slave, unsigned int pc, unsigned int address,
	unsigned int value, int size, int is_write)
{
	int is_instruction_fetch = (address == pc);
	if (is_instruction_fetch)
		return;

	unsigned int uncached_address = address & ~0x20000000u;
	if (checking_hle)
		hud_note_sh2_access(is_slave, address, value, size, is_write);
	if (!logging_traffic || !in_access_ranges(uncached_address))
		return;

	print_dreq_block();   /* keep the log in time order */
	printf("A %06d %03d %s %08x %c%d %08x %0*x\n", current_frame, Pico.m.scanline,
		writer_names[1 + is_slave], pc, is_write ? 'W' : 'R', size, address, size * 2, value);
}

static int shown_framebuffer(void)
{
	return Pico32x.vdp_regs[0x0a / 2] & P32XV_FS;
}

static int display_mode_32x(void)
{
	return Pico32x.vdp_regs[0] & P32XV_Mx;
}

/* ---- Checking the HLE (-h) ---- */

static struct {
	int pictures, identical, saved;
	double lowest;
	int lowest_frame;
} hle_results = { 0, 0, 0, 100.0, -1 };

/* -h: the smooth motion between ticks (hle_interpolated), at its end, must give the
   newest scene's polygon list exactly */
static struct {
	int checked, different;
} interpolation_results;

static int same_polygon(const hle_polygon_t *a, const hle_polygon_t *b)
{
	if (a->colour != b->colour || a->vertex_count != b->vertex_count || a->drawn_with != b->drawn_with)
		return 0;
	for (int n = 0; n < a->vertex_count; n++)
		if (a->x[n] != b->x[n] || a->y[n] != b->y[n] || a->fx[n] != b->fx[n] || a->fy[n] != b->fy[n])
			return 0;
	return 1;
}

static void check_interpolation_end(void)
{
	const hle_frame_t *latest = hle_latest();
	if (latest == NULL)
		return;
	const hle_frame_t *smooth = hle_interpolated(1e9, NULL, 0);   /* long after the newest scene */
	int same = smooth != NULL && smooth->count == latest->count;
	for (int i = 0; same && i < latest->count; i++)
		same = same_polygon(&smooth->polygons[i], &latest->polygons[i]);
	interpolation_results.checked++;
	if (!same)
		interpolation_results.different++;
}

static void save_hle_difference(int frame, const hle_frame_t *hle, const uint8_t *hle_image);
static void save_hle_polygons(int frame, const hle_frame_t *hle);

/* -h: how far the exact vertices (fx, fy) lie from the SH2's (x, y), on screen */
static struct {
	long count, far_off;
	double sum_x, sum_y, largest;
} vertex_offsets;

static void note_vertex_offsets(const hle_frame_t *hle)
{
	for (int i = 0; i < hle->count; i++) {
		const hle_polygon_t *polygon = &hle->polygons[i];
		for (int n = 0; n < polygon->vertex_count; n++) {
			if (polygon->x[n] < -160 || polygon->x[n] > 160 || polygon->y[n] < -112 || polygon->y[n] > 112)
				continue;   /* off screen */
			double dx = polygon->fx[n] - polygon->x[n], dy = polygon->fy[n] - polygon->y[n];
			vertex_offsets.count++;
			vertex_offsets.sum_x += dx;
			vertex_offsets.sum_y += dy;
			double distance = sqrt(dx * dx + dy * dy);
			if (distance > vertex_offsets.largest)
				vertex_offsets.largest = distance;
			if (distance > 2)
				vertex_offsets.far_off++;
		}
	}
}

static void check_hle_picture(int frame, const hle_frame_t *hle)
{
	note_vertex_offsets(hle);
	check_interpolation_end();
	static uint8_t image[SH2_SCREEN_WIDTH * SH2_SCREEN_HEIGHT];
	memset(image, 0, sizeof(image));
	for (int i = 0; i < hle->count; i++)
		sh2_draw_polygon(image, &hle->polygons[i], hle->view_top, hle->view_bottom);

	int compared, same;
	hlecheck_compare(hle, image, &compared, &same);
	printf("H %06d polygons %d pixels %d same %d\n", frame, hle->count, compared, same);

	double percent = compared ? 100.0 * same / compared : 100.0;
	hle_results.pictures++;
	if (same == compared)
		hle_results.identical++;
	else if (hle_results.saved < 3) {
		save_hle_difference(frame, hle, image);
		save_hle_polygons(frame, hle);
		hle_results.saved++;
	}
	if (percent < hle_results.lowest) {
		hle_results.lowest = percent;
		hle_results.lowest_frame = frame;
	}
}

static void print_hle_summary(void)
{
	if (vertex_offsets.count)
		printf("exact vertices minus the SH2's, on screen: mean x %+.3f y %+.3f, largest %.2f pixels, "
			"%ld of %ld more than 2 pixels away\n", vertex_offsets.sum_x / vertex_offsets.count,
			vertex_offsets.sum_y / vertex_offsets.count, vertex_offsets.largest, vertex_offsets.far_off,
			vertex_offsets.count);
	printf("smooth motion at a tick's end vs the newest scene's list: %d of %d differ\n",
		interpolation_results.different, interpolation_results.checked);
	printf("HLE: %d pictures compared, %d identical, lowest %.2f%% (frame %d)\n",
		hle_results.pictures, hle_results.identical, hle_results.lowest, hle_results.lowest_frame);
}

/* ---- Files ---- */

static FILE *open_output(const char *path)
{
	FILE *file = fopen(path, "wb");
	if (file == NULL) {
		perror(path);
		exit(1);
	}
	return file;
}

/* Writes a PPM header; the caller then writes width * height RGB triples. */
static FILE *open_ppm(const char *path, int width, int height)
{
	FILE *file = open_output(path);
	fprintf(file, "P6\n%d %d\n255\n", width, height);
	return file;
}

static void rgb565_to_rgb888(uint16_t pixel, unsigned char rgb[3])
{
	int red   = (pixel >> 11) & 0x1f;   /* 5 bits */
	int green = (pixel >> 5) & 0x3f;    /* 6 bits */
	int blue  = pixel & 0x1f;           /* 5 bits */

	/* Shift up to the 0-255 range. */
	rgb[0] = red << 3;
	rgb[1] = green << 2;
	rgb[2] = blue << 3;
}

/* 32X colours are BGR555: red in bits 0-4, green 5-9, blue 10-14, bit 15 = priority. */
static void bgr555_to_rgb888(uint16_t colour, unsigned char rgb[3])
{
	rgb[0] = (colour & 0x1f) << 3;
	rgb[1] = ((colour >> 5) & 0x1f) << 3;
	rgb[2] = ((colour >> 10) & 0x1f) << 3;
}

/* -h: the frame buffer on screen, with pixels where the HLE differs in magenta */
static void save_hle_difference(int frame, const hle_frame_t *hle, const uint8_t *hle_image)
{
	char path[40];
	snprintf(path, sizeof(path), "hlediff_%06d.ppm", frame);
	FILE *file = open_ppm(path, SH2_SCREEN_WIDTH, SH2_SCREEN_HEIGHT);
	for (int row = 0; row < SH2_SCREEN_HEIGHT; row++) {
		for (int x = 0; x < SH2_SCREEN_WIDTH; x++) {
			int theirs;
			int compared = hlecheck_is_compared(hle, hle_image, x, row, &theirs);
			unsigned char rgb[3];
			bgr555_to_rgb888(Pico32xMem->pal[theirs], rgb);
			if (compared && hle_image[row * SH2_SCREEN_WIDTH + x] != theirs) {
				rgb[0] = 255;
				rgb[1] = 0;
				rgb[2] = 255;
			}
			fwrite(rgb, 1, sizeof(rgb), file);
		}
	}
	fclose(file);
}

/* -h: the HLE's polygon list as text, one polygon per line in drawing order:
   colour word, then screen x y of each vertex (origin top left, y down) */
static void save_hle_polygons(int frame, const hle_frame_t *hle)
{
	char path[40];
	snprintf(path, sizeof(path), "hlepoly_%06d.txt", frame);
	FILE *file = open_output(path);
	for (int i = 0; i < hle->count; i++) {
		const hle_polygon_t *polygon = &hle->polygons[i];
		fprintf(file, "%04x", polygon->colour);
		for (int n = 0; n < polygon->vertex_count; n++)
			fprintf(file, " %d %d", (int16_t)(160 + polygon->x[n]), (int16_t)(112 - polygon->y[n]));
		fprintf(file, "\n");
	}
	fclose(file);
}

/* -w: the finished picture as frame_NNNNNN.ppm */
static void save_screenshot(int frame)
{
	char path[32];
	snprintf(path, sizeof(path), "frame_%06d.ppm", frame);
	FILE *file = open_ppm(path, CORE_SCREEN_WIDTH, core_visible_lines);

	for (int row = 0; row < core_visible_lines; row++) {
		const uint16_t *pixels = &core_screen[(core_visible_top + row) * CORE_SCREEN_WIDTH];
		for (int column = 0; column < CORE_SCREEN_WIDTH; column++) {
			unsigned char rgb[3];
			rgb565_to_rgb888(pixels[column], rgb);
			fwrite(rgb, 1, sizeof(rgb), file);
		}
	}
	fclose(file);
}

/* PicoDrive keeps SH2 memory as 16-bit words in host byte order. This writes it in the
   SH2's (big-endian) byte order, so file offset N holds the byte at SH2 address base + N. */
static void save_words_big_endian(const char *path, const uint16_t *words, size_t word_count)
{
	FILE *file = open_output(path);
	for (size_t i = 0; i < word_count; i++) {
		unsigned char bytes[2] = { words[i] >> 8, words[i] & 0xff };
		fwrite(bytes, 1, 2, file);
	}
	fclose(file);
}

/* One 32X frame buffer decoded as packed pixel (one byte per pixel, a palette index).
   The buffer starts with a table of 256 line offsets, in words from the buffer start;
   the left pixel of each pair is the high byte. */
static void save_32x_framebuffer(int frame, int buffer)
{
	const uint16_t *dram = Pico32xMem->dram[buffer];
	const uint16_t *palette = Pico32xMem->pal;
	const int width = 320, height = 224;

	char path[40];
	snprintf(path, sizeof(path), "fb32x_%06d_%d.ppm", frame, buffer);
	FILE *file = open_ppm(path, width, height);

	for (int row = 0; row < height; row++) {
		uint16_t line_start = dram[row];
		for (int column = 0; column < width; column++) {
			uint16_t pixel_pair = dram[(uint16_t)(line_start + column / 2)];
			int is_left_pixel = (column % 2 == 0);
			int colour_index = is_left_pixel ? (pixel_pair >> 8) : (pixel_pair & 0xff);

			unsigned char rgb[3];
			bgr555_to_rgb888(palette[colour_index], rgb);
			fwrite(rgb, 1, sizeof(rgb), file);
		}
	}
	fclose(file);
}

/* -d: SDRAM, both SH2 caches and both frame buffers */
static void save_32x_memory(int frame)
{
	if (Pico32xMem == NULL)
		return;

	char path[32];
	snprintf(path, sizeof(path), "sdram_%06d.bin", frame);
	save_words_big_endian(path, (const uint16_t *)Pico32xMem->sdram, sizeof(Pico32xMem->sdram) / 2);

	snprintf(path, sizeof(path), "cache_msh2_%06d.bin", frame);
	save_words_big_endian(path, (const uint16_t *)sh2s[0].data_array, sizeof(sh2s[0].data_array) / 2);
	snprintf(path, sizeof(path), "cache_ssh2_%06d.bin", frame);
	save_words_big_endian(path, (const uint16_t *)sh2s[1].data_array, sizeof(sh2s[1].data_array) / 2);

	for (int buffer = 0; buffer < 2; buffer++) {
		snprintf(path, sizeof(path), "dram_%06d_%d.bin", frame, buffer);
		save_words_big_endian(path, Pico32xMem->dram[buffer], sizeof(Pico32xMem->dram[buffer]) / 2);
	}
	snprintf(path, sizeof(path), "pal32x_%06d.bin", frame);
	save_words_big_endian(path, Pico32xMem->pal, sizeof(Pico32xMem->pal) / 2);

	save_32x_framebuffer(frame, 0);
	save_32x_framebuffer(frame, 1);
}

/* -v: Mega Drive VDP and 32X VDP state */
static void save_vdp_state(int frame)
{
	char path[32];
	snprintf(path, sizeof(path), "vdp_%06d.bin", frame);
	FILE *file = open_output(path);

	fwrite("VDPX", 1, 4, file);
	fwrite(Pico.video.reg, 1, 0x20, file);

	/* VRAM is kept as 16-bit words in host byte order, like the SH2 memory. */
	for (size_t i = 0; i < sizeof(PicoMem.vram) / 2; i++) {
		unsigned char bytes[2] = { PicoMem.vram[i] >> 8, PicoMem.vram[i] & 0xff };
		fwrite(bytes, 1, 2, file);
	}
	const uint16_t *word_arrays[] = { PicoMem.cram, PicoMem.vsram, Pico32x.vdp_regs };
	const size_t word_counts[] = { 0x40, 0x40, 0x10 };
	for (int array = 0; array < 3; array++) {
		for (size_t i = 0; i < word_counts[array]; i++) {
			uint16_t word = word_arrays[array][i];
			unsigned char bytes[2] = { word >> 8, word & 0xff };
			fwrite(bytes, 1, 2, file);
		}
	}
	fclose(file);
}

/* ---- Command line ---- */

/* Parses "-p frame:keys". Exits on a malformed argument. */
static struct button_change parse_button_change(const char *argument)
{
	struct button_change change;
	char letters[16] = "";
	if (sscanf(argument, "%d:%15s", &change.frame, letters) < 1) {
		fprintf(stderr, "bad -p argument '%s', expected frame:keys\n", argument);
		exit(1);
	}
	change.buttons = core_parse_buttons(letters);
	return change;
}

static void print_usage(const char *program)
{
	fprintf(stderr, "usage: %s [-n frames] [-p frame:keys]... [-w frame]... [-c frame] [-a start:end]..."
		" [-m start:end]... [-s percent] [-d frame]... [-v frame]... [-h frame] rom.32x\n", program);
}

static int list_contains(const int *list, int count, int value)
{
	for (int i = 0; i < count; i++) {
		if (list[i] == value)
			return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int frame_count = 600;
	struct button_change script[MAX_SCRIPT_ENTRIES];
	int script_length = 0;
	int screenshot_frames[MAX_SCRIPT_ENTRIES];
	int screenshot_count = 0;
	int dump_frames[MAX_SCRIPT_ENTRIES];
	int dump_count = 0;
	int vdp_frames[MAX_SCRIPT_ENTRIES];
	int vdp_count = 0;
	int first_logged_frame = -1;   /* -1 = no traffic log */
	int first_checked_frame = -1;  /* -1 = no HLE check */
	int sh2_percent = 100;

	int option;
	while ((option = getopt(argc, argv, "n:p:w:c:a:m:s:d:v:h:")) != -1) {
		if (option == 'n') {
			frame_count = atoi(optarg);
		} else if (option == 'p' && script_length < MAX_SCRIPT_ENTRIES) {
			script[script_length] = parse_button_change(optarg);
			script_length++;
		} else if (option == 'w' && screenshot_count < MAX_SCRIPT_ENTRIES) {
			screenshot_frames[screenshot_count] = atoi(optarg);
			screenshot_count++;
		} else if (option == 'c') {
			first_logged_frame = atoi(optarg);
		} else if (option == 'a' && access_range_count < MAX_ACCESS_RANGES) {
			unsigned int start, end;
			if (sscanf(optarg, "%x:%x", &start, &end) != 2) {
				print_usage(argv[0]);
				return 1;
			}
			access_ranges[access_range_count].start = start;
			access_ranges[access_range_count].end = end;
			access_range_count++;
		} else if (option == 'm' && ram_range_count < MAX_RAM_RANGES) {
			unsigned int start, end;
			if (sscanf(optarg, "%x:%x", &start, &end) != 2 || end <= start) {
				print_usage(argv[0]);
				return 1;
			}
			ram_ranges[ram_range_count].start = (start & 0xFFFF) & ~1u;
			ram_ranges[ram_range_count].end = ((end - 1) & 0xFFFF) + 1;
			ram_range_count++;
		} else if (option == 's') {
			sh2_percent = atoi(optarg);
		} else if (option == 'd' && dump_count < MAX_SCRIPT_ENTRIES) {
			dump_frames[dump_count] = atoi(optarg);
			dump_count++;
		} else if (option == 'h') {
			first_checked_frame = atoi(optarg);
		} else if (option == 'v' && vdp_count < MAX_SCRIPT_ENTRIES) {
			vdp_frames[vdp_count] = atoi(optarg);
			vdp_count++;
		} else {
			print_usage(argv[0]);
			return 1;
		}
	}
	if (optind != argc - 1) {
		print_usage(argv[0]);
		return 1;
	}
	const char *rom_path = argv[optind];

	if (core_init(rom_path, 44100) != 0)
		return 1;
	if (sh2_percent != 100) {
		int hz = (int)((double)PICO_MSH2_HZ * sh2_percent / 100);
		Pico32xSetClocks(hz, hz);
	}

	uint16_t buttons = 0;
	for (current_frame = 0; current_frame < frame_count; current_frame++) {
		for (int i = 0; i < script_length; i++) {
			if (script[i].frame == current_frame)
				buttons = script[i].buttons;
		}

		int logging = first_logged_frame >= 0 && current_frame >= first_logged_frame;
		logging_traffic = logging;
		/* The HLE follows the game from a few ticks before the check starts. */
		checking_hle = first_checked_frame >= 0 && current_frame >= first_checked_frame - 12;
		vrde_comm_write_hook = logging || checking_hle ? on_comm_write : NULL;
		vrde_dreq_write_hook = logging || checking_hle ? on_dreq_write : NULL;
		int logging_accesses = logging && access_range_count > 0;
		vrde_sh2_access_hook = logging_accesses || checking_hle ? on_sh2_access : NULL;

		core_run_frame(buttons);

		if (logging) {
			print_dreq_block();
			printf("S %06d shown %d mode %d\n", current_frame, shown_framebuffer(), display_mode_32x());
		}
		print_frame_hashes(current_frame);
		if (first_logged_frame < 0 || current_frame >= first_logged_frame)
			print_ram_ranges(current_frame);
		if (checking_hle) {
			const hle_frame_t *picture = hle_on_frame_end();
			if (picture && current_frame >= first_checked_frame)
				check_hle_picture(current_frame, picture);
		}

		if (list_contains(screenshot_frames, screenshot_count, current_frame))
			save_screenshot(current_frame);
		if (list_contains(dump_frames, dump_count, current_frame))
			save_32x_memory(current_frame);
		if (list_contains(vdp_frames, vdp_count, current_frame))
			save_vdp_state(current_frame);
	}
	if (first_checked_frame >= 0)
		print_hle_summary();
	return 0;
}
