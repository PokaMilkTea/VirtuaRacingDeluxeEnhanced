/* Runs the original game on the PicoDrive emulator core.

   The first half of this file holds the functions PicoDrive expects every front end
   to provide (declared in pico/pico.h). The second half is our own interface.
   The start-up steps follow PicoDrive's own libretro front end
   (platform/libretro/libretro.c), keeping only what a 32X game needs. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <stdlib.h>

#include "pico/pico_int.h"
#include "platform/common/input_pico.h"
#include "core.h"

uint16_t core_screen[CORE_SCREEN_WIDTH * CORE_SCREEN_HEIGHT];
int core_visible_top = 0;
int core_visible_lines = 224;
int16_t core_audio_samples[2 * CORE_AUDIO_MAX_FRAMES];
int core_audio_frame_count = 0;

/* ---- Functions PicoDrive calls ---- */

/* Log messages from the core go to stderr. */
void lprintf(const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
}

/* The core tells us which rows of the screen buffer hold the picture. */
void emu_video_mode_change(int start_line, int line_count, int start_column, int column_count)
{
	core_visible_top = start_line;
	core_visible_lines = line_count;
}

/* Chooses how the core outputs the picture. We call it once at start-up; the core
   calls it again when the game switches the 32X on (about 2 frames after power-on).
   PDF_RGB555 lays the 32X layer over the Mega Drive layer; despite the name the
   pixels come out as RGB565. */
void emu_32x_startup(void)
{
	PicoDrawSetOutFormat(PDF_RGB555, 0);

	int bytes_per_row = CORE_SCREEN_WIDTH * sizeof(uint16_t);
	PicoDrawSetOutBuf(core_screen, bytes_per_row);
}

/* Memory for the core's large buffers. The requested address is only a hint, and the
   core accepts memory anywhere else. */
void *plat_mmap(unsigned long address, size_t size, int need_exec, int is_fixed)
{
#ifdef _WIN32
	/* page-aligned and zeroed, as mmap's */
	return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
	void *memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED)
		return NULL;
	return memory;
#endif
}

/* Grows or shrinks a buffer from plat_mmap, keeping its contents. */
void *plat_mremap(void *old_memory, size_t old_size, size_t new_size)
{
	void *new_memory = plat_mmap(0, new_size, 0, 0);
	if (new_memory == NULL)
		return NULL;

	size_t bytes_to_keep = old_size;
	if (new_size < old_size)
		bytes_to_keep = new_size;
	memcpy(new_memory, old_memory, bytes_to_keep);

	plat_munmap(old_memory, old_size);
	return new_memory;
}

void plat_munmap(void *memory, size_t size)
{
	if (memory == NULL)
		return;
#ifdef _WIN32
	VirtualFree(memory, 0, MEM_RELEASE);
#else
	munmap(memory, size);
#endif
}

/* The SH2 dynarec (recompiler) is off: the SH2 interpreter is fast enough. These three
   are only used by the dynarec.
   ponytail: turning the dynarec on for macOS arm64 needs MAP_JIT memory here and
   pthread_jit_write_protect_np around code writes. */
void *plat_mem_get_for_drc(size_t size)
{
	return NULL;
}

int plat_mem_set_exec(void *memory, size_t size)
{
	return -1;
}

void cache_flush_d_inval_i(void *start, void *end)
{
}

/* MP3 and OGG audio tracks only exist on Mega CD discs, never on a 32X cartridge. */
int mp3_get_bitrate(void *file, int size) { return -1; }
void mp3_start_play(void *file, int position) {}
void mp3_update(s32 *buffer, int length, int stereo) {}
int ogg_get_length(void *file) { return -1; }
void ogg_start_play(void *file, int sample_offset) {}
void ogg_stop_play(void) {}
void ogg_update(s32 *buffer, int length, int stereo) {}

/* ---- Our interface ---- */

/* The core mixes each frame's sound here, then calls write_sound. */
static int16_t core_mix_buffer[2 * CORE_AUDIO_MAX_FRAMES];

/* Called by the core at the end of every frame. The core clears core_mix_buffer as soon
   as this returns (PsndGetSamples in pico/sound/sound.c), so copy the samples now. */
static void write_sound(int byte_count)
{
	int bytes_per_stereo_frame = 2 * sizeof(int16_t);
	core_audio_frame_count = byte_count / bytes_per_stereo_frame;
	memcpy(core_audio_samples, core_mix_buffer, byte_count);
}

/* Options that must be set before the core starts. */
static void set_core_options(int sample_rate)
{
	PicoIn.opt = POPT_EN_STEREO      /* stereo sound */
		| POPT_EN_FM             /* Mega Drive FM sound chip (YM2612) */
		| POPT_EN_PSG            /* Mega Drive square-wave sound chip (SN76489) */
		| POPT_EN_Z80            /* the Z80 sound CPU */
		| POPT_EN_32X            /* the 32X add-on */
		| POPT_EN_PWM            /* 32X sound */
		| POPT_ACC_SPRITES       /* accurate sprite priority */
		| POPT_DIS_32C_BORDER;   /* no black border in 256-pixel-wide modes */
	PicoIn.sndRate = sample_rate;
	PicoIn.autoRgnOrder = 0x184; /* if the ROM allows several regions: USA, then Europe, then Japan */
}

static void set_up_sound(void)
{
	PicoIn.sndOut = core_mix_buffer;
	PicoIn.writeSound = write_sound;
	PsndRerate(0);
}

int core_init(const char *rom_path, int sample_rate)
{
	set_core_options(sample_rate);
	PicoInit();

	enum media_type_e media = PicoLoadMedia(rom_path, NULL, 0, NULL, NULL, NULL, NULL);
	if (media != PM_MD_CART) {
		fprintf(stderr, "cannot load %s\n", rom_path);
		return -1;
	}

	PicoSetInputDevice(0, PICO_INPUT_PAD_6BTN);
	PicoLoopPrepare();
	set_up_sound();
	emu_32x_startup();
	return 0;
}

void core_run_frame(uint16_t buttons)
{
	PicoIn.pad[0] = buttons;
	core_audio_frame_count = 0;
	PicoFrame();
}

uint16_t core_parse_buttons(const char *letters)
{
	uint16_t buttons = 0;
	for (const char *letter = letters; *letter != '\0'; letter++) {
		int button;
		switch (*letter) {
		case 'S': button = GBTN_START; break;
		case 'A': button = GBTN_A;     break;
		case 'B': button = GBTN_B;     break;
		case 'C': button = GBTN_C;     break;
		case 'U': button = GBTN_UP;    break;
		case 'D': button = GBTN_DOWN;  break;
		case 'L': button = GBTN_LEFT;  break;
		case 'R': button = GBTN_RIGHT; break;
		case 'X': button = GBTN_X;     break;
		case 'Y': button = GBTN_Y;     break;
		case 'Z': button = GBTN_Z;     break;
		case 'M': button = GBTN_MODE;  break;
		default:
			fprintf(stderr, "unknown button '%c'\n", *letter);
			exit(1);
		}
		buttons |= 1 << button;
	}
	return buttons;
}

void core_button_letters(uint16_t buttons, char *letters)
{
	static const struct { char letter; int button; } order[] = {
		{ 'S', GBTN_START }, { 'A', GBTN_A }, { 'B', GBTN_B }, { 'C', GBTN_C },
		{ 'U', GBTN_UP }, { 'D', GBTN_DOWN }, { 'L', GBTN_LEFT }, { 'R', GBTN_RIGHT },
		{ 'X', GBTN_X }, { 'Y', GBTN_Y }, { 'Z', GBTN_Z }, { 'M', GBTN_MODE },
	};
	for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
		if (buttons & (1 << order[i].button))
			*letters++ = order[i].letter;
	}
	*letters = '\0';
}

int core_save_state(const char *path)
{
	return PicoState(path, 1) == 0;
}

int core_load_state(const char *path)
{
	return PicoState(path, 0) == 0;
}
