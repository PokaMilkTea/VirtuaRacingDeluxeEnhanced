/* The patch table: every change to the game's logic goes through it. When the 68000 is
   about to run a patched address, a C function runs first and returns the address to go on
   from (the same address runs the 68000's own code). Nothing in the ROM is rewritten. */
#ifndef VRDE_M68KPATCH_H
#define VRDE_M68KPATCH_H

#include "cpu/fame/fame.h"

typedef unsigned int (*m68k_patch_function)(M68K_CONTEXT *cpu, unsigned int pc);

/* Adds a patch; a second one at the same address replaces the first */
void m68k_patch_add(unsigned int address, m68k_patch_function function);

/* Removes every patch: the 68000 then runs the game as it is */
void m68k_patch_clear(void);

#endif
