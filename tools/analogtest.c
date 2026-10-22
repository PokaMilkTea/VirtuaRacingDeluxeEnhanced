/* A check of the analog controls' mapping (src/analog.c): make analogtest */
#include <assert.h>
#include <stdio.h>
#include "analog.h"
#include "m68kpatch.h"

/* the core's side of the patch table, which this check does not link */
unsigned int (*vrde_m68k_patch_hook)(M68K_CONTEXT *, unsigned int);
unsigned int vrde_m68k_patch_low, vrde_m68k_patch_span;

int main(void)
{
	/* the stick: left positive, 1 just out of the dead zone, the full target at the end */
	assert(analog_stick_target(0, 8) == 0);
	assert(analog_stick_target(2621, 8) == 0);      /* the edge of the dead zone */
	assert(analog_stick_target(2622, 8) == -1);     /* just out: the smallest right */
	assert(analog_stick_target(-2622, 8) == 1);
	assert(analog_stick_target(32767, 8) == -127);
	assert(analog_stick_target(-32768, 8) == 127);
	assert(analog_stick_target(16384, 0) == -64);
	int last = 0;
	for (int axis = 0; axis <= 32767; axis++) {     /* never more than a step at a time */
		int target = -analog_stick_target(axis, 8);
		assert(target >= last && target - last <= 1);
		last = target;
	}

	/* a trigger: 0 in the dead zone, then 1 to 255 */
	assert(analog_trigger_amount(0, 5) == 0);
	assert(analog_trigger_amount(1638, 5) == 0);
	assert(analog_trigger_amount(1639, 5) == 1);
	assert(analog_trigger_amount(32767, 5) == ANALOG_PEDAL_MAX);
	assert(analog_trigger_amount(-5, 5) == 0);
	last = 0;
	for (int axis = 0; axis <= 32767; axis++) {
		int amount = analog_trigger_amount(axis, 5);
		assert(amount >= last && amount - last <= 1);
		last = amount;
	}
	puts("analog mapping ok");
	return 0;
}
