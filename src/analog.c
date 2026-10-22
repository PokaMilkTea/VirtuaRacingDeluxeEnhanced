/* Analog controls (analog.h), through the patch table. See "Steering and the replay
   record" and "Throttle and brake" in docs/game-loop.md.

   The steering (the 68000's 8894FA): once per tick it turns the pad's left and right into a
   target from -127 to 127 in steps of 24, then moves the car's actual steering halfway to it.
   The stick replaces the first part, the target, and leaves the averaging to the game.

   The pedals (88961E, after the steering): with the throttle (B) or the brake (A) held, the
   pedal goes 48 up a tick, otherwise 48 down: the throttle between -51 and 255, the brake
   between 0 and 255. A trigger gives the pedal a place to go to instead, at the same 48 a
   tick: from 0 just out of its dead zone to 255 fully pressed, as with the button held. The
   throttle's -51 (the engine braking) is left to the trigger released: from about -51 to 10
   a pedal hardly does anything (the push stays under the resistance), and the buttons only
   ever pass through it.

   The game's replay keeps only the pad's buttons and simulates the race again from them, so
   the analog controls of every tick are kept here as well (by the tick's place in the game's
   record) and played back in the replay. With every control in its dead zone, the game's own
   code runs: the same as the original. */
#include <stdio.h>
#include <string.h>

#include "analog.h"
#include "m68kpatch.h"

/* ---- the game's code and memory ---- */

#define STEERING_CODE        0x8894FA   /* where the 68000's steering starts */
#define AVERAGING_CODE       0x8895BC   /* its part after the target, run as it is */
#define PEDALS_CODE          0x88961E   /* where the pedals start */
#define PEDALS_END           0x889686   /* their rts */
#define RECORD_CODE          0x884D3A   /* the race keeps this tick's buttons for the replay */
#define PLAYBACK_CODE        0x88568A   /* the replay takes this tick's buttons back */
#define STEP_TABLE           0x8894F6   /* -24, 0, +24 at offsets -2, 0, +2 */

#define TARGET               0xFFC000   /* word, -127..127 */
#define LAST_DIRECTION       0xFFC006   /* word: 1 left, -1 right, 0 neither or both */
#define PAD_THIS_TICK        0xFFC300   /* byte: the held buttons ($C971) of this tick */
#define PAD_LAST_TICK        0xFFC301   /*  and of the last one */
#define PAD_HELD             0xFFC971
#define SLIDE_FLAG           0xFFC8C8   /* word: not 0 = the step below halves (meaning unknown) */
#define SWAP_LEFT_RIGHT      0xFFFDA8   /* byte: bit 7 swaps left and right */
#define THROTTLE_LIMIT       0xFFC0AC   /* word: below the car's $92, the throttle only lifts */
#define RECORD_POINTER       0xFFC8C0   /* word: the next tick's byte of the game's record */
#define RECORD_START         0xC9A0     /* the first tick's byte */

/* In the car's structure (a0) */
#define CAR_THROTTLE         0x0E       /* word, -51..255 */
#define CAR_BRAKE            0x10       /* word, 0..255 */
#define CAR_NO_PEDALS_1      0x6A       /* either not 0: the pedals stay as they are */
#define CAR_NO_PEDALS_2      0x8C
#define CAR_STEERING         0x8E       /* target x 256, averaged */
#define CAR_THROTTLE_LIMIT   0x92
#define CAR_SLIDE            0x94       /* the word the slide flag's case takes 1/8 off */

#define LEFT_BIT             2
#define RIGHT_BIT            3
#define THROTTLE_BIT         4
#define BRAKE_BIT            6

#define PEDAL_STEP           48
#define THROTTLE_LOWEST      -51
#define BRAKE_LOWEST         0

/* The 68000's cycles (FAME's count) for the parts replaced: 326 to 376 for the steering's
   target, 258 to 290 for the pedals. Charged as one number each, so the C versions are not
   cycle exact. */
#define TARGET_CYCLES        334
#define PEDALS_CYCLES        270

/* ---- the record ---- */

/* The analog controls of each tick of the game's record (as long as it: $C9A0-$EF00);
   0 = the pad's buttons worked that control */
#define RECORD_TICKS         (0xEF00 - RECORD_START)
typedef struct {
	signed char steering;
	unsigned char throttle, brake;
} controls;
static controls record[RECORD_TICKS];

static controls live;   /* analog_set_input */

/* The controls of the tick under way, taken as the race keeps its buttons (the live ones,
   kept for the replay) or as the replay takes them back (those kept), and used until the
   pedals, the last of the tick's controls, are done. Modes without a replay (Free Run)
   don't record: they use the live ones. */
static int tick_recorded;
static controls tick_controls;

static controls this_tick(void)
{
	return tick_recorded ? tick_controls : live;
}

/* The tick's place in the game's record, -1 past its end (the game keeps no more) */
static int record_index(M68K_CONTEXT *cpu)
{
	int index = (int)cpu->read_word(RECORD_POINTER) - RECORD_START;
	return index >= 0 && index < RECORD_TICKS ? index : -1;
}

static unsigned int on_record(M68K_CONTEXT *cpu, unsigned int pc)
{
	int index = record_index(cpu);
	if (index >= 0)
		record[index] = live;
	tick_controls = live;
	tick_recorded = 1;
	return pc;
}

static unsigned int on_playback(M68K_CONTEXT *cpu, unsigned int pc)
{
	int index = record_index(cpu);
	controls none = { 0, 0, 0 };
	tick_controls = index >= 0 ? record[index] : none;
	tick_recorded = 1;
	return pc;
}

/* ---- the steering ---- */

static int sign_extend(unsigned int word)
{
	return (int)(short)word;
}

/* 1 for left, -1 for right, 0 for neither or both */
static int direction_of(unsigned int pad, int left_bit, int right_bit)
{
	return ((pad >> left_bit) & 1) - ((pad >> right_bit) & 1);
}

static int step_for(M68K_CONTEXT *cpu, int direction)
{
	return sign_extend(cpu->read_word(STEP_TABLE + 2 * direction));
}

/* The pad's bookkeeping at the start of the steering: this tick's buttons kept, and the
   direction they give */
static int read_direction(M68K_CONTEXT *cpu)
{
	cpu->write_byte(PAD_LAST_TICK, cpu->read_byte(PAD_THIS_TICK));
	unsigned int pad = cpu->read_byte(PAD_HELD);
	cpu->write_byte(PAD_THIS_TICK, pad);
	int swapped = cpu->read_byte(SWAP_LEFT_RIGHT) & 0x80;
	return swapped ? direction_of(pad, RIGHT_BIT, LEFT_BIT) : direction_of(pad, LEFT_BIT, RIGHT_BIT);
}

/* The new target from the pad, as 8894FA-8895BA computes it. May change the car's steering. */
static int pad_target(M68K_CONTEXT *cpu, unsigned int car, int direction)
{
	int target = sign_extend(cpu->read_word(TARGET));
	if (direction != sign_extend(cpu->read_word(LAST_DIRECTION))) {
		/* a new direction (or a release): straight to the first step, and the car's
		   steering with it */
		cpu->write_word(LAST_DIRECTION, direction);
		target = step_for(cpu, direction);
		cpu->write_word(car + CAR_STEERING, target * 256);
		return target;
	}
	if (direction == 0) {
		/* still released: a target left over goes back one step toward 0 */
		int toward_zero = target == 0 ? 0 : target > 0 ? 1 : -1;
		return target - step_for(cpu, toward_zero);
	}
	/* the same direction held: one more step */
	cpu->write_word(LAST_DIRECTION, direction);
	int step = step_for(cpu, direction);
	if (cpu->read_word(SLIDE_FLAG) != 0) {
		int slide = sign_extend(cpu->read_word(car + CAR_SLIDE));
		if (((slide ^ step) & 0x8000) == 0) {
			step >>= 1;
			cpu->write_word(car + CAR_SLIDE, slide - (slide >> 3));
		}
	}
	return target + step;
}

/* Stores the target and goes on with the game's averaging, leaving the registers as the
   68000's code does (those the rest does not set again) */
static unsigned int finish_target(M68K_CONTEXT *cpu, int target)
{
	if (target > ANALOG_STEERING_MAX)
		target = ANALOG_STEERING_MAX;
	if (target < -ANALOG_STEERING_MAX)
		target = -ANALOG_STEERING_MAX;
	cpu->write_word(TARGET, target);
	cpu->areg[1].D = STEP_TABLE;
	cpu->dreg[0].D = 0;
	cpu->io_cycle_counter -= TARGET_CYCLES;
	return AVERAGING_CODE;
}

static unsigned int analog_steering(M68K_CONTEXT *cpu, unsigned int pc)
{
	int stick = this_tick().steering;
	if (stick == 0)
		return pc;   /* the game's own steering */
	/* the pad's bookkeeping goes on as usual, so that the pad takes over smoothly when the
	   stick comes back into its dead zone */
	int direction = read_direction(cpu);
	cpu->write_word(LAST_DIRECTION, direction);
	return finish_target(cpu, stick);
}

/* -x: the pad's target in C, on every tick */
static unsigned int c_pad_steering(M68K_CONTEXT *cpu, unsigned int pc)
{
	unsigned int car = cpu->areg[0].D;
	int direction = read_direction(cpu);
	return finish_target(cpu, pad_target(cpu, car, direction));
}

/* ---- the pedals ---- */

/* A pedal 48 closer to where it goes, and within its range */
static int pedal_toward(int pedal, int goal, int lowest)
{
	if (goal > pedal + PEDAL_STEP)
		goal = pedal + PEDAL_STEP;
	if (goal < pedal - PEDAL_STEP)
		goal = pedal - PEDAL_STEP;
	if (goal > ANALOG_PEDAL_MAX)
		goal = ANALOG_PEDAL_MAX;
	if (goal < lowest)
		goal = lowest;
	return goal;
}

/* Where a pedal goes this tick: with the trigger in its dead zone (amount 0), all the way up
   with its button held and all the way down otherwise; a trigger from 0 (just out of the
   dead zone, amount 1) to the top (fully pressed) */
static int pedal_goal(int amount, int button_held, int lowest)
{
	if (amount == 0)
		return button_held ? ANALOG_PEDAL_MAX : lowest;
	return (amount - 1) * ANALOG_PEDAL_MAX / (ANALOG_PEDAL_MAX - 1);
}

/* The pedals as 88961E-889684 move them, the triggers' amounts (0 = the buttons) taken in */
static unsigned int run_pedals(M68K_CONTEXT *cpu, controls input)
{
	unsigned int car = cpu->areg[0].D;
	unsigned int pad = cpu->read_byte(PAD_HELD);
	int throttle = sign_extend(cpu->read_word(car + CAR_THROTTLE));
	int brake = sign_extend(cpu->read_word(car + CAR_BRAKE));

	int lifting = sign_extend(cpu->read_word(THROTTLE_LIMIT)) < sign_extend(cpu->read_word(car + CAR_THROTTLE_LIMIT));
	int throttle_goal = lifting ? THROTTLE_LOWEST
	                            : pedal_goal(input.throttle, (pad >> THROTTLE_BIT) & 1, THROTTLE_LOWEST);
	throttle = pedal_toward(throttle, throttle_goal, THROTTLE_LOWEST);
	int brake_goal = pedal_goal(input.brake, (pad >> BRAKE_BIT) & 1, BRAKE_LOWEST);
	int braking = brake_goal > brake || (brake_goal == brake && brake_goal != BRAKE_LOWEST);
	brake = pedal_toward(brake, brake_goal, BRAKE_LOWEST);

	cpu->write_word(car + CAR_THROTTLE, throttle);
	cpu->write_word(car + CAR_BRAKE, brake);
	/* the registers as the 68000's code leaves them: d0 the brake's step, +48 pressing and
	   -48 not (a word), the low word of d1 the brake */
	cpu->dreg[0].D = (braking ? PEDAL_STEP : -PEDAL_STEP) & 0xFFFF;
	cpu->dreg[1].D = (cpu->dreg[1].D & 0xFFFF0000) | (brake & 0xFFFF);
	cpu->io_cycle_counter -= PEDALS_CYCLES;
	return PEDALS_END;
}

static int pedals_held(M68K_CONTEXT *cpu)
{
	unsigned int car = cpu->areg[0].D;
	return (cpu->read_word(car + CAR_NO_PEDALS_1) | cpu->read_word(car + CAR_NO_PEDALS_2)) != 0;
}

static unsigned int analog_pedals(M68K_CONTEXT *cpu, unsigned int pc)
{
	controls input = this_tick();
	tick_recorded = 0;   /* the tick's last control */
	if ((input.throttle == 0 && input.brake == 0) || pedals_held(cpu))
		return pc;   /* the game's own pedals */
	return run_pedals(cpu, input);
}

/* -x: the pad's pedals in C, on every tick */
static unsigned int c_pad_pedals(M68K_CONTEXT *cpu, unsigned int pc)
{
	controls buttons_only = { 0, 0, 0 };
	if (pedals_held(cpu))
		return pc;
	return run_pedals(cpu, buttons_only);
}

/* ---- the program's side ---- */

void analog_install(void)
{
	m68k_patch_add(RECORD_CODE, on_record);
	m68k_patch_add(PLAYBACK_CODE, on_playback);
	m68k_patch_add(STEERING_CODE, analog_steering);
	m68k_patch_add(PEDALS_CODE, analog_pedals);
}

void analog_install_c_pad(void)
{
	m68k_patch_add(STEERING_CODE, c_pad_steering);
	m68k_patch_add(PEDALS_CODE, c_pad_pedals);
}

void analog_set_input(int steering, int throttle, int brake)
{
	live.steering = (signed char)steering;
	live.throttle = (unsigned char)throttle;
	live.brake = (unsigned char)brake;
}

/* The way beyond the dead zone (out of 32767) as 1 to most */
static int beyond_dead_zone(int magnitude, int dead_zone_percent, int most)
{
	int dead = 32767 * dead_zone_percent / 100;
	if (magnitude > 32767)
		magnitude = 32767;
	if (magnitude <= dead)
		return 0;
	int value = 1 + (magnitude - dead - 1) * most / (32767 - dead);
	return value > most ? most : value;
}

int analog_stick_target(int axis, int dead_zone_percent)
{
	int target = beyond_dead_zone(axis < 0 ? -axis : axis, dead_zone_percent, ANALOG_STEERING_MAX);
	return axis < 0 ? target : -target;   /* the stick's left (negative) is the game's left (positive) */
}

int analog_trigger_amount(int axis, int dead_zone_percent)
{
	return beyond_dead_zone(axis < 0 ? 0 : axis, dead_zone_percent, ANALOG_PEDAL_MAX);
}

void analog_clear_record(void)
{
	memset(record, 0, sizeof(record));
	tick_recorded = 0;
}

int analog_save_record(const char *path)
{
	FILE *file = fopen(path, "wb");
	if (!file)
		return 0;
	int written = fwrite(record, 1, sizeof(record), file) == sizeof(record);
	return fclose(file) == 0 && written;
}

int analog_load_record(const char *path)
{
	FILE *file = fopen(path, "rb");
	int read = 0;
	if (file) {
		read = fread(record, 1, sizeof(record), file) == sizeof(record) && fgetc(file) == EOF;
		fclose(file);
	}
	if (!read)
		analog_clear_record();
	tick_recorded = 0;
	return read;
}
