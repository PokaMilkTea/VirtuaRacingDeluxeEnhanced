/* sh2dis: disassembles SH2 code from a binary file (the ROM, or an SDRAM dump from
   vrdetrace -d), using PicoDrive's copy of MAME's SH2 disassembler.

   usage: sh2dis file base_address start_address end_address
     base_address   the SH2 address of the file's first byte, e.g. 06000000 for an SDRAM
                    dump, or 02000000 for the ROM (the SH2 sees the cartridge there)
     start/end      the SH2 address range to disassemble (end not included)
   All addresses in hex.

   Output: one line per instruction, "address  opcode  instruction". For loads of a
   constant from memory near the code (MOV.W / MOV.L @(disp,PC)), the loaded value is
   shown after "=". */
#include <stdio.h>
#include <stdlib.h>

#include "cpu/sh2/mame/sh2dasm.h"

static unsigned char *file_data;
static long file_size;
static unsigned long base_address;

static void load_file(const char *path)
{
	FILE *file = fopen(path, "rb");
	if (file == NULL) {
		perror(path);
		exit(1);
	}
	fseek(file, 0, SEEK_END);
	file_size = ftell(file);
	fseek(file, 0, SEEK_SET);

	file_data = malloc(file_size);
	if (fread(file_data, 1, file_size, file) != (size_t)file_size) {
		fprintf(stderr, "cannot read %s\n", path);
		exit(1);
	}
	fclose(file);
}

static int address_is_in_file(unsigned long address, int size)
{
	return address >= base_address && address + size <= base_address + file_size;
}

/* The SH2 is big-endian. */
static unsigned read16(unsigned long address)
{
	const unsigned char *bytes = &file_data[address - base_address];
	return (bytes[0] << 8) | bytes[1];
}

static unsigned long read32(unsigned long address)
{
	return ((unsigned long)read16(address) << 16) | read16(address + 2);
}

/* For "MOV.W @(disp,PC),Rn" and "MOV.L @(disp,PC),Rn", prints the constant loaded. */
static void print_pc_relative_constant(unsigned long address, unsigned opcode)
{
	unsigned displacement = opcode & 0xff;
	int is_word_load = (opcode & 0xf000) == 0x9000;
	int is_long_load = (opcode & 0xf000) == 0xd000;

	if (is_word_load) {
		unsigned long constant_address = address + 4 + displacement * 2;
		if (address_is_in_file(constant_address, 2))
			printf("  = %04x", read16(constant_address));
	}
	if (is_long_load) {
		unsigned long constant_address = ((address + 4) & ~3ul) + displacement * 4;
		if (address_is_in_file(constant_address, 4))
			printf("  = %08lx", read32(constant_address));
	}
}

int main(int argc, char **argv)
{
	if (argc != 5) {
		fprintf(stderr, "usage: %s file base_address start_address end_address (hex)\n", argv[0]);
		return 1;
	}
	load_file(argv[1]);
	base_address = strtoul(argv[2], NULL, 16);
	unsigned long start = strtoul(argv[3], NULL, 16) & ~1ul;
	unsigned long end = strtoul(argv[4], NULL, 16);

	for (unsigned long address = start; address < end; address += 2) {
		if (!address_is_in_file(address, 2))
			break;
		unsigned opcode = read16(address);

		/* Pass the instruction's own address; DasmSH2 adds the 2 itself. */
		char text[128];
		DasmSH2(text, address, opcode);

		printf("%08lx  %04x  %s", address, opcode, text);
		print_pc_relative_constant(address, opcode);
		printf("\n");
	}
	return 0;
}
