/* The patch table (m68kpatch.h), on the hook in FAME's instruction loop (fame.h). The core
   checks only whether the address is in the range of all patches; the list is searched
   here, and only for addresses in that range. */
#include <stdio.h>
#include <stdlib.h>

#include "m68kpatch.h"

#define MAX_PATCHES 16

static struct {
	unsigned int address;
	m68k_patch_function function;
} patches[MAX_PATCHES];
static int patch_count;

static unsigned int run_patch(M68K_CONTEXT *cpu, unsigned int pc)
{
	for (int i = 0; i < patch_count; i++) {
		if (patches[i].address == pc)
			return patches[i].function(cpu, pc);
	}
	return pc;
}

/* The range the core checks: from the lowest patched address to the highest */
static void update_range(void)
{
	if (patch_count == 0) {
		vrde_m68k_patch_hook = NULL;
		return;
	}
	unsigned int low = patches[0].address, high = patches[0].address;
	for (int i = 1; i < patch_count; i++) {
		if (patches[i].address < low)
			low = patches[i].address;
		if (patches[i].address > high)
			high = patches[i].address;
	}
	vrde_m68k_patch_low = low;
	vrde_m68k_patch_span = high - low + 1;
	vrde_m68k_patch_hook = run_patch;
}

void m68k_patch_add(unsigned int address, m68k_patch_function function)
{
	int i = 0;
	while (i < patch_count && patches[i].address != address)
		i++;
	if (i == MAX_PATCHES) {
		fprintf(stderr, "too many patches\n");
		exit(1);
	}
	if (i == patch_count)
		patch_count++;
	patches[i].address = address;
	patches[i].function = function;
	update_range();
}

void m68k_patch_clear(void)
{
	patch_count = 0;
	update_range();
}
