#ifndef SYSHM_ROM_H
#define SYSHM_ROM_H

#include <stddef.h>
#include <stdint.h>

#include "parcels.h"

/* The Toolbox ROM image is always 4 MB and is mapped at this guest address. */
#define ROM_IMAGE_SIZE 0x400000u
#define ROM_BASE 0xFFC00000u

/* The 68k-style checksum stored at offset 0 covers [4, 3 MB) as 16-bit words. */
#define ROM_CHECKSUM_END 0x300000u

/* NUL-terminated NanoKernel/boot identification string, e.g. "NewWorld v1.0". */
#define ROM_NANOKERNEL_ID_OFFSET 0x30D064u

enum rom_format {
	ROM_FORMAT_RAW,         /* 4 MB OldWorld ROM dump */
	ROM_FORMAT_BOOTINFO,    /* NewWorld "Mac OS ROM" file with an LZSS Toolbox image */
	ROM_FORMAT_PARCELS,     /* ... with parcels (Mac OS 9.1 on), the Toolbox image one of them */
};

struct rom {
	enum rom_format format;

	uint8_t *file;          /* raw file contents */
	size_t file_size;

	uint8_t *image;         /* decoded Toolbox ROM image, ROM_IMAGE_SIZE bytes */

	/* NewWorld bootinfo fields (zero for raw images) */
	char compatible[256];   /* models from <COMPATIBLE>, space separated */
	uint32_t elf_offset, elf_size;      /* Trampoline (OF client program) */
	uint32_t lzss_offset, lzss_size;    /* compressed Toolbox image, or the parcels */
	uint32_t elf_entry;
	struct parcels parcels;             /* ROM_FORMAT_PARCELS */
};

/* Returns 0 on success; on failure returns -1 and writes a message to err. */
int rom_load(const char *path, struct rom *rom, char *err, size_t errlen);
void rom_free(struct rom *rom);

uint32_t rom_stored_checksum(const struct rom *rom);
uint32_t rom_computed_checksum(const struct rom *rom);
uint16_t rom_version(const struct rom *rom);
uint16_t rom_release(const struct rom *rom);
uint32_t rom_resource_map_offset(const struct rom *rom);
uint32_t rom_trap_table_offset(const struct rom *rom);

/* Copies the NanoKernel identification string into buf (always terminated). */
void rom_nanokernel_id(const struct rom *rom, char *buf, size_t buflen);

#endif
