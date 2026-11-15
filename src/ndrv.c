#include "ndrv.h"

#include <stdlib.h>
#include <string.h>

#include "be.h"
#include "ppc.h"
#include "ppc_fp.h"

/* PEF section kinds and sharing */
#define SEC_CODE 0
#define SEC_DATA 1
#define SEC_LOADER 4
#define SHARE_PROCESS 1

/* Symbol classes */
#define SYM_DATA 1
#define SYM_TVECTOR 2

/* Relocation opcodes (16-bit instructions; runs of 1 to RELOC_RUN) */
#define RELOC_IMPORT_RUN(n) (0x4A00u | ((n) - 1u))
#define RELOC_TVECTOR8(n) (0x4600u | ((n) - 1u))
#define RELOC_RUN 512u
#define RELOC_INCR_POSITION(bytes) (0x8000u | ((bytes) - 1u))

#define HEADER_SIZE 40u
#define SECTION_HEADER_SIZE 28u
#define LOADER_HEADER_SIZE 56u
#define DESCRIPTION_SIZE 0x80u
#define FRAME_SIZE NDRV_FRAME_SIZE

uint32_t pef_hash_word(const char *name, size_t len)
{
	int32_t h = 0;
	for (size_t k = 0; k < len; k++)
		h = (int32_t)(((uint32_t)h << 1) - (uint32_t)(h >> 16)) ^ (uint8_t)name[k];
	return (uint32_t)len << 16 | (uint16_t)(h ^ (h >> 16));
}

uint32_t ndrv_arg(const struct ndrv_call *c, unsigned k)
{
	if (k < 8)
		return c->arg[k];
	uint32_t at = c->frame + NDRV_FRAME_SIZE + 24 + 4 * k;
	return mem_ok(c->mem, at, 4, MEM_R) ? mem_r32(c->mem, at) : 0;
}

uint32_t ndrv_next_word(struct ndrv_args *a)
{
	return ndrv_arg(a->c, a->word++);
}

uint32_t ndrv_next_float(struct ndrv_args *a)
{
	if (a->fpr >= 13)
		return ndrv_next_word(a);
	a->word++;
	return ppc_fp_double_to_single(a->c->f[a->fpr++]);
}

uint64_t ndrv_next_double(struct ndrv_args *a)
{
	if (a->fpr >= 13) {
		uint64_t hi = ndrv_next_word(a);
		return hi << 32 | ndrv_next_word(a);
	}
	a->word += 2;
	return a->c->f[a->fpr++];
}

static size_t align(size_t v, size_t a)
{
	return (v + a - 1) & ~(a - 1);
}

static void put_pstring(uint8_t *p, const char *s, size_t max)
{
	size_t n = strlen(s);
	if (n > max - 1)
		n = max - 1;
	p[0] = (uint8_t)n;
	memcpy(p + 1, s, n);
}

static void section_header(uint8_t *h, uint32_t size, uint32_t total, uint32_t offset, uint8_t kind,
			   uint8_t align_pow)
{
	put_be32(h + 0, 0xFFFFFFFFu);           /* no name */
	put_be32(h + 4, 0);                     /* default address */
	put_be32(h + 8, kind == SEC_LOADER ? 0 : total);
	put_be32(h + 12, kind == SEC_LOADER ? 0 : size);
	put_be32(h + 16, size);
	put_be32(h + 20, offset);
	h[24] = kind;
	h[25] = SHARE_PROCESS;
	h[26] = align_pow;
	h[27] = 0;
}

/* Entry k at 8 * k: li r11,k ; b common; then the fragment's own code. Returns the code size; writes it if `p`. */
static size_t make_code(uint8_t *p, uint32_t hostcall, unsigned entries, const uint32_t *code, size_t code_words)
{
	const uint32_t common[] = {
		0x7C0802A6u,                    /* common:  mflr r0 */
		0x90010008u,                    /*          stw r0,8(r1) */
		0x94210000u | (uint16_t)-FRAME_SIZE,    /* stwu r1,-FRAME(r1) */
		0x90410014u,                    /*          stw r2,20(r1) */
		PPC_HOSTCALL_INSN(hostcall),    /* loop:    hostcall */
		0x41820020u,                    /*          beq done */
		0x800C0000u,                    /*          lwz r0,0(r12) */
		0x804C0004u,                    /*          lwz r2,4(r12) */
		0x7C0903A6u,                    /*          mtctr r0 */
		0x4E800421u,                    /*          bctrl */
		0x80410014u,                    /*          lwz r2,20(r1) */
		0x3960FFFFu,                    /*          li r11,-1 */
		0x4BFFFFE0u,                    /*          b loop */
		0x80010000u | (FRAME_SIZE + 8), /* done:    lwz r0,FRAME+8(r1) */
		0x38210000u | FRAME_SIZE,       /*          addi r1,r1,FRAME */
		0x7C0803A6u,                    /*          mtlr r0 */
		0x4E800020u,                    /*          blr */
	};
	const size_t ncommon = sizeof common / sizeof common[0];
	_Static_assert(sizeof common == NDRV_COMMON_SIZE, "ndrv.h's NDRV_COMMON_SIZE");
	if (p) {
		for (unsigned k = 0; k < entries; k++) {
			put_be32(p + 8 * k, 0x39600000u | k);
			put_be32(p + 8 * k + 4, 0x48000000u | ((8 * (entries - k) - 4) & 0x03FFFFFCu));
		}
		for (size_t k = 0; k < ncommon; k++)
			put_be32(p + 8 * entries + 4 * k, common[k]);
		for (size_t k = 0; k < code_words; k++)
			put_be32(p + ndrv_code_offset(entries) + 4 * k, code[k]);
	}
	return ndrv_code_offset(entries) + 4 * code_words;
}

/* An export, and its chain in the hash table */
struct export {
	const char *name;
	uint8_t sym_class;
	uint32_t value;
	uint32_t key;                           /* hash word */
	unsigned slot;
};

static int by_slot(const void *a, const void *b)
{
	const struct export *p = a, *q = b;
	return p->slot != q->slot ? (p->slot < q->slot ? -1 : 1) : 0;
}

/* As PEF linkers size it: chains of about 10 (PEFBinaryFormat.h's kExponentLimit and kAverageChainLimit) */
static unsigned hash_power(unsigned exports)
{
	unsigned p = 16;
	while (p > 0 && exports / (1u << p) < 10)
		p--;
	return p;
}

size_t ndrv_build(const struct ndrv_spec *spec, uint8_t *out, size_t size, struct ndrv_layout *layout)
{
	const unsigned nimp = spec->import_count, entries = spec->entries ? spec->entries : NDRV_ENTRIES;
	const bool driver = spec->kind == FRAG_DRIVER;
	const unsigned nexports = driver ? 2 : spec->kind == FRAG_LIBRARY ? spec->export_count : 0;

	if ((driver && nimp == 0) || nimp > NDRV_MAX_IMPORTS || entries > NDRV_MAX_ENTRIES ||
	    nexports > NDRV_MAX_ENTRIES * 2 || entries > 0x7FFF)
		return 0;
	const size_t code_size = make_code(NULL, 0, entries, NULL, spec->code_words);
	for (unsigned k = 0; spec->entry_code && k < entries; k++)
		if (spec->entry_code[k] != NDRV_STUB && (spec->entry_code[k] >= code_size || spec->entry_code[k] % 4))
			return 0;

	/* Libraries: runs of imports with the same library name */
	const char *lib_name[NDRV_MAX_IMPORTS];
	unsigned lib_first[NDRV_MAX_IMPORTS], lib_count[NDRV_MAX_IMPORTS], nlibs = 0;
	for (unsigned k = 0; k < nimp; k++) {
		if (nlibs && strcmp(lib_name[nlibs - 1], spec->imports[k].library) == 0) {
			lib_count[nlibs - 1]++;
		} else {
			lib_name[nlibs] = spec->imports[k].library;
			lib_first[nlibs] = k;
			lib_count[nlibs] = 1;
			nlibs++;
		}
	}

	/* Data section layout */
	memset(layout, 0, sizeof *layout);
	for (unsigned k = 0; k < nimp; k++)
		layout->import[k] = 4 * k;
	const uint32_t tvectors = (uint32_t)align(4 * nimp, 8);
	for (unsigned k = 0; k < entries; k++)
		layout->entry[k] = tvectors + 8 * k;
	const uint32_t description = tvectors + 8 * entries;
	layout->extra = description + (driver ? DESCRIPTION_SIZE : 0);

	/* Exports, sorted into the hash table's chains */
	struct export *exp = calloc(nexports ? nexports : 1, sizeof *exp);
	if (!exp)
		return 0;
	const unsigned power = hash_power(nexports);
	for (unsigned k = 0; k < nexports; k++) {
		struct export *e = &exp[k];
		bool data = driver ? k == 1 : spec->exports[k].data;
		e->name = driver ? (k ? "TheDriverDescription" : "DoDriverIO") : spec->exports[k].name;
		e->sym_class = data ? SYM_DATA : SYM_TVECTOR;
		if (driver) {
			e->value = k ? description : layout->entry[0];
		} else if (data) {
			if (spec->exports[k].offset >= spec->extra_len + spec->zero_len) {
				free(exp);
				return 0;
			}
			e->value = layout->extra + spec->exports[k].offset;
		} else {
			if (spec->exports[k].entry >= entries) {
				free(exp);
				return 0;
			}
			e->value = layout->entry[spec->exports[k].entry];
		}
		e->key = pef_hash_word(e->name, strlen(e->name));
		e->slot = (e->key ^ e->key >> power) & ((1u << power) - 1);
	}
	/* (a stable sort: qsort isn't, so the original order breaks ties) */
	for (unsigned k = 0; k < nexports; k++)
		exp[k].slot = exp[k].slot << 16 | k;
	qsort(exp, nexports, sizeof *exp, by_slot);
	for (unsigned k = 0; k < nexports; k++)
		exp[k].slot >>= 16;

	uint16_t relocs[2 + (NDRV_MAX_ENTRIES + RELOC_RUN - 1) / RELOC_RUN];
	size_t nrelocs = 0;
	_Static_assert(NDRV_MAX_IMPORTS <= RELOC_RUN, "imports in one run");
	if (nimp)
		relocs[nrelocs++] = (uint16_t)RELOC_IMPORT_RUN(nimp);
	if (tvectors > 4 * nimp)
		relocs[nrelocs++] = (uint16_t)RELOC_INCR_POSITION(tvectors - 4 * nimp);
	for (unsigned k = 0; k < entries; k += RELOC_RUN)
		relocs[nrelocs++] = (uint16_t)RELOC_TVECTOR8(entries - k < RELOC_RUN ? entries - k : RELOC_RUN);

	const size_t code_off = align(HEADER_SIZE + 3 * SECTION_HEADER_SIZE, 16);
	const size_t data_off = align(code_off + code_size, 16);
	const size_t data_size = layout->extra + spec->extra_len;
	const size_t loader_off = align(data_off + data_size, 16);

	/* Loader section layout */
	const size_t lib_tab = LOADER_HEADER_SIZE;
	const size_t imp_tab = lib_tab + 24 * nlibs;
	const size_t reloc_hdr = imp_tab + 4 * nimp;
	const size_t reloc_instr = reloc_hdr + 12;
	const size_t strings = align(reloc_instr + 2 * nrelocs, 4);
	size_t strings_size = 0;
	for (unsigned k = 0; k < nlibs; k++)
		strings_size += strlen(lib_name[k]) + 1;
	for (unsigned k = 0; k < nimp; k++)
		strings_size += strlen(spec->imports[k].name) + 1;
	for (unsigned k = 0; k < nexports; k++)
		strings_size += strlen(exp[k].name);
	const size_t hash_tab = align(strings + strings_size, 4);
	const size_t key_tab = hash_tab + 4 * ((size_t)1 << power);
	const size_t sym_tab = key_tab + nexports * 4;
	const size_t loader_size = sym_tab + nexports * 10;
	const size_t total = loader_off + loader_size;

	if (total > size || code_size > 0x7FFFFFF0u) {
		free(exp);
		return 0;
	}
	memset(out, 0, total);

	/* Container header */
	memcpy(out, "Joy!peffpwpc", 12);
	put_be32(out + 12, 1);                  /* format version */
	put_be32(out + 20, spec->old_def_version);
	put_be32(out + 28, spec->current_version);
	put_be16(out + 32, 3);                  /* sections */
	put_be16(out + 34, 2);                  /* instantiated sections */
	section_header(out + HEADER_SIZE, (uint32_t)code_size, (uint32_t)code_size, (uint32_t)code_off, SEC_CODE, 2);
	section_header(out + HEADER_SIZE + SECTION_HEADER_SIZE, (uint32_t)data_size,
		       (uint32_t)data_size + spec->zero_len, (uint32_t)data_off, SEC_DATA, 3);
	section_header(out + HEADER_SIZE + 2 * SECTION_HEADER_SIZE, (uint32_t)loader_size, 0, (uint32_t)loader_off,
		       SEC_LOADER, 0);

	make_code(out + code_off, spec->hostcall, entries, spec->code, spec->code_words);

	/* Data: entry TVectors (code offsets; relocated), description, driver data */
	uint8_t *data = out + data_off;
	for (unsigned k = 0; k < entries; k++)
		put_be32(data + layout->entry[k], spec->entry_code && spec->entry_code[k] != NDRV_STUB ?
						   spec->entry_code[k] : 8 * k);
	if (driver) {
		uint8_t *d = data + description;
		memcpy(d, "mtej", 4);
		put_be32(d + 4, 0);                     /* description version */
		put_pstring(d + 8, spec->device_name, 32);
		put_be32(d + 40, spec->version);
		put_be32(d + 44, spec->runtime_options);
		put_pstring(d + 48, spec->driver_name, 32);
		put_be32(d + 112, 1);                   /* services */
		put_be32(d + 116, spec->category);
		put_be32(d + 120, spec->type);
		put_be32(d + 124, spec->version);
	}
	if (spec->extra_len)
		memcpy(data + layout->extra, spec->extra, spec->extra_len);

	/* Loader */
	uint8_t *l = out + loader_off;
	put_be32(l + 0, 0xFFFFFFFFu);           /* no main */
	put_be32(l + 8, 0xFFFFFFFFu);           /* no init */
	put_be32(l + 16, 0xFFFFFFFFu);          /* no term */
	if (spec->kind == FRAG_APPLICATION) {   /* main: entry 0 */
		put_be32(l + 0, 1);
		put_be32(l + 4, layout->entry[0]);
	} else if (spec->kind == FRAG_LIBRARY) {
		if (spec->init_entry >= 0 && (unsigned)spec->init_entry < entries) {
			put_be32(l + 8, 1);
			put_be32(l + 12, layout->entry[spec->init_entry]);
		}
		if (spec->term_entry && spec->term_entry < entries) {
			put_be32(l + 16, 1);
			put_be32(l + 20, layout->entry[spec->term_entry]);
		}
	}
	put_be32(l + 24, nlibs);
	put_be32(l + 28, nimp);
	put_be32(l + 32, 1);                    /* sections with relocations */
	put_be32(l + 36, (uint32_t)reloc_instr);
	put_be32(l + 40, (uint32_t)strings);
	put_be32(l + 44, (uint32_t)hash_tab);
	put_be32(l + 48, power);
	put_be32(l + 52, nexports);

	size_t s = 0;
	uint8_t *str = l + strings;
	for (unsigned k = 0; k < nlibs; k++) {
		uint8_t *e = l + lib_tab + 24 * k;
		put_be32(e + 0, (uint32_t)s);
		put_be32(e + 12, lib_count[k]);
		put_be32(e + 16, lib_first[k]);
		memcpy(str + s, lib_name[k], strlen(lib_name[k]) + 1);
		s += strlen(lib_name[k]) + 1;
	}
	for (unsigned k = 0; k < nimp; k++) {
		put_be32(l + imp_tab + 4 * k, (uint32_t)SYM_TVECTOR << 24 | (uint32_t)s);
		memcpy(str + s, spec->imports[k].name, strlen(spec->imports[k].name) + 1);
		s += strlen(spec->imports[k].name) + 1;
	}

	put_be16(l + reloc_hdr + 0, 1);         /* data section */
	put_be32(l + reloc_hdr + 4, (uint32_t)nrelocs);
	put_be32(l + reloc_hdr + 8, 0);
	for (size_t k = 0; k < nrelocs; k++)
		put_be16(l + reloc_instr + 2 * k, relocs[k]);

	/* the hash table: each chain's count and first export; the exports in chain order */
	for (unsigned k = 0, first = 0; k < 1u << power; k++) {
		unsigned n = 0;
		while (first + n < nexports && exp[first + n].slot == k)
			n++;
		put_be32(l + hash_tab + 4 * k, n << 18 | first);
		first += n;
	}
	for (unsigned k = 0; k < nexports; k++) {
		size_t n = strlen(exp[k].name);
		memcpy(str + s, exp[k].name, n);
		put_be32(l + key_tab + 4 * k, exp[k].key);
		uint8_t *e = l + sym_tab + 10 * k;
		put_be32(e, (uint32_t)exp[k].sym_class << 24 | (uint32_t)s);
		put_be32(e + 4, exp[k].value);
		put_be16(e + 8, 1);
		s += n;
	}
	free(exp);
	return total;
}
