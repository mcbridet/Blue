#include "rom.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "be.h"
#include "lzss.h"

static int fail(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
	return -1;
}

static int read_file(const char *path, uint8_t **data, size_t *size, char *err, size_t errlen)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return fail(err, errlen, "%s: %s", path, strerror(errno));

	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return fail(err, errlen, "%s: cannot seek", path);
	}
	long len = ftell(f);
	rewind(f);
	if (len <= 0) {
		fclose(f);
		return fail(err, errlen, "%s: empty or unreadable", path);
	}

	/* (a disk image given by mistake isn't read whole) */
	char head[11];
	if (len != ROM_IMAGE_SIZE &&
	    (fread(head, 1, sizeof head, f) != sizeof head || memcmp(head, "<CHRP-BOOT>", sizeof head))) {
		fclose(f);
		return fail(err, errlen, "%s: not a 4 MB ROM image or a NewWorld \"Mac OS ROM\" file", path);
	}
	rewind(f);

	uint8_t *buf = malloc((size_t)len);
	if (!buf) {
		fclose(f);
		return fail(err, errlen, "%s: out of memory", path);
	}
	if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
		free(buf);
		fclose(f);
		return fail(err, errlen, "%s: short read", path);
	}
	fclose(f);

	*data = buf;
	*size = (size_t)len;
	return 0;
}

static const uint8_t *find_bytes(const uint8_t *hay, size_t haylen, const char *needle)
{
	size_t n = strlen(needle);

	if (n == 0 || n > haylen)
		return NULL;
	for (size_t i = 0; i + n <= haylen; i++) {
		if (hay[i] == (uint8_t)needle[0] && memcmp(hay + i, needle, n) == 0)
			return hay + i;
	}
	return NULL;
}

static int is_word_char(int c)
{
	return isalnum(c) || c == '-' || c == '_';
}

/* Parses "<hex> constant <name>" from the bootinfo Forth script. */
static int script_constant(const uint8_t *script, size_t len, const char *name, uint32_t *value)
{
	char needle[64];
	snprintf(needle, sizeof needle, " constant %s", name);

	const uint8_t *end = script + len;
	const uint8_t *p = script;
	for (;;) {
		p = find_bytes(p, (size_t)(end - p), needle);
		if (!p)
			return -1;
		const uint8_t *after = p + strlen(needle);
		if (after >= end || !is_word_char(*after))
			break;
		p++;
	}

	const uint8_t *digits_end = p;
	const uint8_t *digits = p;
	while (digits > script && isxdigit(digits[-1]))
		digits--;
	if (digits == digits_end || digits_end - digits > 8)
		return -1;

	uint32_t v = 0;
	for (const uint8_t *q = digits; q < digits_end; q++)
		v = v * 16 + (uint32_t)(isdigit(*q) ? *q - '0' : tolower(*q) - 'a' + 10);
	*value = v;
	return 0;
}

static void parse_compatible(const uint8_t *script, size_t len, char *out, size_t outlen)
{
	const char *open = "<COMPATIBLE>";
	const uint8_t *p = find_bytes(script, len, open);
	out[0] = '\0';
	if (!p)
		return;
	p += strlen(open);
	const uint8_t *e = find_bytes(p, len - (size_t)(p - script), "</COMPATIBLE>");
	if (!e)
		return;

	size_t o = 0;
	int pending_space = 0;
	for (; p < e && o + 2 < outlen; p++) {
		if (isspace(*p)) {
			pending_space = o > 0;
			continue;
		}
		if (pending_space)
			out[o++] = ' ';
		pending_space = 0;
		out[o++] = (char)*p;
	}
	out[o] = '\0';
}

static int load_bootinfo(struct rom *rom, const char *path, char *err, size_t errlen)
{
	const uint8_t *file = rom->file;
	size_t size = rom->file_size;

	const uint8_t *script_end = find_bytes(file, size < 0x20000 ? size : 0x20000, "</CHRP-BOOT>");
	if (!script_end)
		return fail(err, errlen, "%s: bootinfo header has no </CHRP-BOOT>", path);
	size_t script_len = (size_t)(script_end - file);

	parse_compatible(file, script_len, rom->compatible, sizeof rom->compatible);

	/* (the parcels of Mac OS 9.1's ROM file are where the boot script says its LZSS image is) */
	if ((script_constant(file, script_len, "parcels-offset", &rom->lzss_offset) ||
	     script_constant(file, script_len, "parcels-size", &rom->lzss_size)) &&
	    (script_constant(file, script_len, "lzss-offset", &rom->lzss_offset) ||
	     script_constant(file, script_len, "lzss-size", &rom->lzss_size)))
		return fail(err, errlen, "%s: boot script has no lzss-offset/lzss-size", path);
	if (rom->lzss_offset > size || rom->lzss_size > size - rom->lzss_offset)
		return fail(err, errlen, "%s: compressed image lies outside the file", path);

	if (script_constant(file, script_len, "elf-offset", &rom->elf_offset) == 0 &&
	    script_constant(file, script_len, "elf-size", &rom->elf_size) == 0 &&
	    rom->elf_offset <= size && rom->elf_size <= size - rom->elf_offset && rom->elf_size >= 52) {
		const uint8_t *elf = file + rom->elf_offset;
		/* 32-bit, big-endian ELF */
		if (memcmp(elf, "\x7f" "ELF", 4) == 0 && elf[4] == 1 && elf[5] == 2)
			rom->elf_entry = be32(elf + 24);
	} else {
		rom->elf_offset = rom->elf_size = 0;
	}

	const uint8_t *packed = file + rom->lzss_offset;
	if (parcels_detect(packed, rom->lzss_size)) {
		char why[200];
		if (parcels_parse(packed, rom->lzss_size, &rom->parcels, why, sizeof why))
			return fail(err, errlen, "%s: %s", path, why);
		const struct parcel_blob *image = parcels_find(&rom->parcels, "rom ", "rom ");
		if (!image || image->size != ROM_IMAGE_SIZE)
			return fail(err, errlen, "%s: no %u-byte Toolbox image among the parcels", path, ROM_IMAGE_SIZE);
		memcpy(rom->image, image->data, ROM_IMAGE_SIZE);
		rom->format = ROM_FORMAT_PARCELS;
		return 0;
	}

	size_t n = lzss_decode(packed, rom->lzss_size, rom->image, ROM_IMAGE_SIZE);
	if (n != ROM_IMAGE_SIZE)
		return fail(err, errlen, "%s: Toolbox image decoded to %zu bytes, expected %u", path, n, ROM_IMAGE_SIZE);

	rom->format = ROM_FORMAT_BOOTINFO;
	return 0;
}

static int load(struct rom *rom, const char *name, char *err, size_t errlen)
{
	rom->image = calloc(1, ROM_IMAGE_SIZE);
	if (!rom->image) {
		rom_free(rom);
		return fail(err, errlen, "out of memory");
	}

	int rc;
	if (rom->file_size == ROM_IMAGE_SIZE) {
		memcpy(rom->image, rom->file, ROM_IMAGE_SIZE);
		rom->format = ROM_FORMAT_RAW;
		rc = 0;
	} else if (rom->file_size > 11 && memcmp(rom->file, "<CHRP-BOOT>", 11) == 0) {
		rc = load_bootinfo(rom, name, err, errlen);
	} else {
		rc = fail(err, errlen, "%s: not a 4 MB ROM image or a NewWorld \"Mac OS ROM\" file", name);
	}

	if (rc)
		rom_free(rom);
	return rc;
}

int rom_load(const char *path, struct rom *rom, char *err, size_t errlen)
{
	memset(rom, 0, sizeof *rom);

	if (read_file(path, &rom->file, &rom->file_size, err, errlen))
		return -1;
	return load(rom, path, err, errlen);
}

int rom_load_mem(const void *data, size_t size, const char *name, struct rom *rom, char *err, size_t errlen)
{
	memset(rom, 0, sizeof *rom);

	rom->file = malloc(size ? size : 1);
	if (!rom->file)
		return fail(err, errlen, "out of memory");
	memcpy(rom->file, data, size);
	rom->file_size = size;
	return load(rom, name, err, errlen);
}

size_t rom_file_version(const struct rom *rom, char *buf, size_t len)
{
	const struct parcel_blob *v = NULL;
	for (unsigned k = 0; k < rom->parcels.count && !v; k++)
		for (unsigned j = 0; j < rom->parcels.list[k].count && !v; j++)
			if (strcmp(rom->parcels.list[k].blobs[j].name, "MacOSROMFile-version") == 0)
				v = &rom->parcels.list[k].blobs[j];
	size_t n = v ? strnlen((const char *)v->data, v->size) : 0;
	if (n >= len)
		n = len - 1;
	memcpy(buf, v ? (const char *)v->data : "", n);
	buf[n] = '\0';
	return n;
}

void rom_free(struct rom *rom)
{
	parcels_free(&rom->parcels);
	free(rom->file);
	free(rom->image);
	memset(rom, 0, sizeof *rom);
}

uint32_t rom_stored_checksum(const struct rom *rom)
{
	return be32(rom->image);
}

uint32_t rom_computed_checksum(const struct rom *rom)
{
	uint32_t sum = 0;
	for (uint32_t o = 4; o < ROM_CHECKSUM_END; o += 2)
		sum += be16(rom->image + o);
	return sum;
}

uint16_t rom_version(const struct rom *rom)
{
	return be16(rom->image + 8);
}

uint16_t rom_release(const struct rom *rom)
{
	return be16(rom->image + 18);
}

uint32_t rom_resource_map_offset(const struct rom *rom)
{
	return be32(rom->image + 26);
}

uint32_t rom_trap_table_offset(const struct rom *rom)
{
	return be32(rom->image + 34);
}

void rom_nanokernel_id(const struct rom *rom, char *buf, size_t buflen)
{
	const uint8_t *s = rom->image + ROM_NANOKERNEL_ID_OFFSET;
	size_t i = 0;

	while (i + 1 < buflen && i < 32 && s[i] >= 0x20 && s[i] < 0x7F) {
		buf[i] = (char)s[i];
		i++;
	}
	buf[i] = '\0';
}
