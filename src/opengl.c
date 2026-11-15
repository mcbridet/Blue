#include "opengl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "be.h"
#include "opengl_table.h"
#include "tools.h"

/* ---- the functions (opengl_table.h) --------------------------------------------- */

struct function {
	const char *name, *result, *args;
	uint16_t offset;                /* in a context's dispatch table (gl) */
};

static const struct function library_functions[] = {
#define X(name, result, args, offset) { #name, result, args, offset },
	OPENGL_LIBRARY_FUNCTIONS(X)
#undef X
};

enum {
#define X(name, result, args, offset) F_##name,
	OPENGL_LIBRARY_FUNCTIONS(X)
#undef X
	F_LIBRARY
};

static const struct function memory_functions[] = {
#define X(name, result, args, offset) { #name, result, args, offset },
	OPENGL_MEMORY_FUNCTIONS(X)
#undef X
};

enum {
#define X(name, result, args, offset) M_##name,
	OPENGL_MEMORY_FUNCTIONS(X)
#undef X
	M_LIBRARY
};

static const struct function utility_functions[] = {
#define X(name, result, args, offset) { #name, result, args, offset },
	OPENGL_UTILITY_FUNCTIONS(X)
#undef X
};

enum {
#define X(name, result, args, offset) U_##name,
	OPENGL_UTILITY_FUNCTIONS(X)
#undef X
	U_LIBRARY
};

#define GL_FUNCTIONS F_aglChoosePixelFormat     /* (gl's first, in the dispatch table's order) */
#define TABLE_SLOTS 441u                        /* the dispatch table's, after the handle: to 0x6E4 */
#define CONTEXT_SIZE 0x700u                     /* (the handle and the table, rounded up) */
_Static_assert(GL_FUNCTIONS == 440 && 4 * TABLE_SLOTS + 4 <= CONTEXT_SIZE, "opengl_table.h");

/*
 * OpenGLLibrary's entries: each function; its initialization and
 * termination routines; one that does nothing (the dispatch table's slot
 * for glBlendFuncSeparate, which Mac OS 9 doesn't have); then each gl
 * function as the dispatch table calls it, with the context's handle first.
 */
enum { E_INIT = F_LIBRARY, E_TERM, E_NOTHING, E_TABLE, E_LIBRARY = E_TABLE + GL_FUNCTIONS };
_Static_assert(E_INIT == OPENGL_ENTRY_INIT && E_TERM == OPENGL_ENTRY_TERM && E_TABLE == OPENGL_ENTRY_TABLE,
	       "opengl.h's entries");

/* ---- OpenGL's and AGL's numbers ------------------------------------------------- */

/* OpenGL's (the front end's are in glfront.h) */
enum { GL_VENDOR = 0x1F00, GL_RENDERER, GL_VERSION, GL_EXTENSIONS };
enum { GL_DOUBLEBUFFER = 0x0C32, GL_MAX_TEXTURE_SIZE = 0x0D33, GL_RED_BITS = 0x0D52, GL_GREEN_BITS, GL_BLUE_BITS,
       GL_ALPHA_BITS, GL_DEPTH_BITS, GL_STENCIL_BITS };

/* AGL's (agl.h) */
enum { AGL_NONE, AGL_ALL_RENDERERS, AGL_BUFFER_SIZE, AGL_LEVEL, AGL_RGBA, AGL_DOUBLEBUFFER, AGL_STEREO,
       AGL_AUX_BUFFERS, AGL_RED_SIZE, AGL_GREEN_SIZE, AGL_BLUE_SIZE, AGL_ALPHA_SIZE, AGL_DEPTH_SIZE,
       AGL_STENCIL_SIZE, AGL_ACCUM_RED_SIZE, AGL_ACCUM_GREEN_SIZE, AGL_ACCUM_BLUE_SIZE, AGL_ACCUM_ALPHA_SIZE,
       AGL_PIXEL_SIZE = 50, AGL_MINIMUM_POLICY, AGL_MAXIMUM_POLICY, AGL_OFFSCREEN, AGL_FULLSCREEN,
       AGL_SAMPLE_BUFFERS_ARB, AGL_SAMPLES_ARB, AGL_RENDERER_ID = 70, AGL_SINGLE_RENDERER, AGL_NO_RECOVERY,
       AGL_ACCELERATED, AGL_CLOSEST_POLICY, AGL_ROBUST, AGL_BACKING_STORE, AGL_MP_SAFE = 78, AGL_WINDOW = 80,
       AGL_MULTISCREEN, AGL_VIRTUAL_SCREEN, AGL_COMPLIANT,
       AGL_BUFFER_MODES = 100, AGL_MIN_LEVEL, AGL_MAX_LEVEL, AGL_COLOR_MODES, AGL_ACCUM_MODES, AGL_DEPTH_MODES,
       AGL_STENCIL_MODES, AGL_MAX_AUX_BUFFERS, AGL_VIDEO_MEMORY = 120, AGL_TEXTURE_MEMORY, AGL_RENDERER_COUNT = 128 };
enum { AGL_SWAP_RECT = 200, AGL_BUFFER_RECT = 202, AGL_SWAP_LIMIT, AGL_COLORMAP_TRACKING = 210,
       AGL_COLORMAP_ENTRY = 212, AGL_RASTERIZATION = 220, AGL_SWAP_INTERVAL = 222, AGL_STATE_VALIDATION = 230 };
enum { AGL_NO_ERROR, AGL_BAD_ATTRIBUTE = 10000, AGL_BAD_PROPERTY, AGL_BAD_PIXELFMT, AGL_BAD_RENDINFO,
       AGL_BAD_CONTEXT, AGL_BAD_DRAWABLE, AGL_BAD_GDEV, AGL_BAD_STATE, AGL_BAD_VALUE, AGL_BAD_MATCH, AGL_BAD_ENUM,
       AGL_BAD_OFFSCREEN, AGL_BAD_FULLSCREEN, AGL_BAD_WINDOW, AGL_BAD_POINTER, AGL_BAD_MODULE, AGL_BAD_ALLOC,
       AGL_ERRORS };
#define AGL_MONOSCOPIC_BIT 0x1u
#define AGL_SINGLEBUFFER_BIT 0x4u
#define AGL_DOUBLEBUFFER_BIT 0x8u
#define AGL_0_BIT 0x1u
#define AGL_8_BIT 0x80u
#define AGL_16_BIT 0x400u
#define AGL_24_BIT 0x800u
#define AGL_32_BIT 0x1000u
#define AGL_RGB555_BIT 0x200u
#define AGL_RGB888_BIT 0x4000u
#define AGL_ARGB8888_BIT 0x8000u

/* AGL's options for aglEnable(), as bits */
enum { OPTION_SWAP_RECT, OPTION_BUFFER_RECT, OPTION_SWAP_LIMIT, OPTION_COLORMAP_TRACKING, OPTION_RASTERIZATION,
       OPTION_STATE_VALIDATION };

/* ---- identity ------------------------------------------------------------------- */

/* As Apple's ATI renderer (see docs/opengl-plan.md, Identity) */
#define RENDERER_ID 0x00021000u
#define VIDEO_MEMORY (32u << 20)
#define TEXTURE_MEMORY (32u << 20)
static const char vendor[] = "ATI Technologies Inc.";
static const char renderer[] = "Rage 128 OpenGL Engine";
static const char version[] = "1.2 syshm";
/* (Mac OS 9.2.2's engine's, in its order, but only those syshm does) */
static const char extensions[] = "GL_APPLE_transform_hint GL_APPLE_packed_pixel GL_ARB_transpose_matrix "
				 "GL_EXT_clip_volume_hint GL_EXT_rescale_normal GL_NV_texgen_reflection "
				 "GL_ARB_multitexture GL_ARB_texture_env_add GL_EXT_blend_color GL_EXT_blend_minmax "
				 "GL_EXT_blend_subtract GL_EXT_compiled_vertex_array GL_EXT_texture_env_add "
				 "GL_EXT_abgr GL_EXT_bgra GL_SGIS_texture_edge_clamp";
/* aglErrorString()'s, as Apple's */
static const char *const agl_errors[AGL_ERRORS - AGL_BAD_ATTRIBUTE] = {
	"invalid pixel format attribute", "invalid renderer property", "invalid pixel format",
	"invalid renderer info", "invalid context", "invalid drawable", "invalid graphics device",
	"invalid context state", "invalid numerical value", "invalid share context", "invalid enumerant",
	"invalid offscreen drawable", "invalid fullscreen drawable", "invalid window", "invalid pointer",
	"invalid code module", "memory allocation failure",
};

/* ---- the library's data ----------------------------------------------------------- */

/*
 * After the entries' TVectors: what it is and its version (which goes up
 * when the fragment changes, so that one installed by an older syshm is
 * told apart), the current context (glc_cc), where the next record goes
 * and where the buffer ends (see "recording"), the strings glGetString()
 * and aglErrorString() give; then, zero-filled, the contexts, pixel
 * formats, renderer infos and the recording buffer.
 */
#define DATA_MAGIC 0x7379474Cu          /* 'syGL' */
#define DATA_VERSION 2u
enum { D_MAGIC = 0, D_VERSION = 4, D_CURRENT = OPENGL_DATA_CURRENT, D_RECORD = OPENGL_DATA_RECORD,
       D_RECORD_END = D_RECORD + 4, D_STRINGS = 20 };
#define FORMATS 16
#define FORMAT_SIZE 32u
#define RENDERER_INFOS 4
#define RENDERER_INFO_SIZE 16u
#define FORMAT_MAGIC 0x73795046u        /* 'syPF' */
#define RENDERER_INFO_MAGIC 0x73795249u /* 'syRI' */
/* A pixel format's words: magic, next, flags, depth, stencil and pixel sizes, its device (a list of one) */
enum { PF_MAGIC = 0, PF_NEXT = 4, PF_FLAGS = 8, PF_DEPTH = 12, PF_STENCIL = 16, PF_PIXEL = 20, PF_DEVICE = 24 };
#define PF_DOUBLE 0x01u
#define PF_WINDOW 0x02u
#define PF_FULLSCREEN 0x04u
#define PF_OFFSCREEN 0x08u
#define PF_NO_RECOVERY 0x10u
#define PF_ALL_RENDERERS 0x20u
#define PF_POLICY_MASK 0x1C0u           /* (minimum 0x40, maximum 0x80, closest 0x100) */

static struct {
	uint8_t data[1024];
	size_t len;
	uint32_t vendor, renderer, version, extensions;
	uint32_t agl_error[AGL_ERRORS - AGL_BAD_ATTRIBUTE], no_error, unknown_error;
	uint32_t contexts, formats, renderer_infos, record, end;       /* the zero-filled parts */
} lib;

static uint32_t put_string(const char *s)
{
	uint32_t at = (uint32_t)lib.len;
	size_t n = strlen(s) + 1;
	memcpy(lib.data + lib.len, s, n);
	lib.len += n;
	return at;
}

static void make_data(void)
{
	if (lib.len)
		return;
	put_be32(lib.data + D_MAGIC, DATA_MAGIC);
	put_be32(lib.data + D_VERSION, DATA_VERSION);
	lib.len = D_STRINGS;
	lib.vendor = put_string(vendor);
	lib.renderer = put_string(renderer);
	lib.version = put_string(version);
	lib.extensions = put_string(extensions);
	lib.no_error = put_string("no error");
	lib.unknown_error = put_string("no such error code");
	for (unsigned k = 0; k < AGL_ERRORS - AGL_BAD_ATTRIBUTE; k++)
		lib.agl_error[k] = put_string(agl_errors[k]);
	lib.contexts = (uint32_t)(lib.len + 15) & ~15u;
	lib.formats = lib.contexts + OPENGL_CONTEXTS * CONTEXT_SIZE;
	lib.renderer_infos = lib.formats + FORMATS * FORMAT_SIZE;
	lib.record = lib.renderer_infos + RENDERER_INFOS * RENDERER_INFO_SIZE;
	lib.end = lib.record + OPENGL_RECORD_SIZE;
}

/* ---- recording ---------------------------------------------------------------------- */

/*
 * gl functions that return nothing and take only values, or values and
 * then one array of a size their name gives (glVertex3fv, glLoadMatrixf),
 * aren't host calls: the library's own code for each (record_code())
 * appends a record of the call to a buffer in the process's instance of
 * the library's data, and the host does what's recorded before any host
 * call of the library's (so before glGetError, a swap, or another context
 * made current). A record is a word, the function's entry << 16 | the
 * record's size in bytes, then the arguments in order: each integer a
 * word, as its register had it (or past r10, the caller's parameter area),
 * each float the 4 bytes stfs stores, each double its 8; an array's
 * elements each as such an argument (bytes and halfwords made words). When
 * the buffer is full, the call is a host call after all, and the host
 * makes the same record of it and does it by the same code. Calls through
 * a context's dispatch table are recorded so too, if that context is the
 * current one.
 *
 * Not recorded: glFlush and glFinish (which show the frame), and what reads
 * arrays in the guest's memory there and then (glArrayElement,
 * glDrawArrays, glDrawElements, glDrawRangeElements, glLockArraysEXT).
 */
#define RECORD_MAX 160u                 /* bytes, at most (glLoadMatrixd's: 132) */
#define CODE_WORDS 32768u               /* the recording code's, at most */

struct recorded {
	uint8_t size;                   /* the record's, in bytes (0: not recorded) */
	uint8_t count;                  /* the array's elements (0: no array) */
	char args[20];                  /* the arguments' types, as recorded */
};
static struct recorded recorded[GL_FUNCTIONS];

/* Attribute functions (glVertex3f, glColor4ubv...): what they set, from how many values of which type */
enum { A_NONE, A_VERTEX, A_COLOR, A_SECONDARY, A_NORMAL, A_TEXCOORD, A_MULTITEX, A_RASTERPOS, A_INDEX, A_EDGEFLAG,
       A_FOGCOORD, A_EVALCOORD };
struct attrib {
	uint8_t kind, count;
	char type;                      /* (as opengl_table.h's) */
	bool array;                     /* (...v: in an array) */
};
static struct attrib attribs[GL_FUNCTIONS];
static bool inside_ok[GL_FUNCTIONS];    /* what may be called between glBegin and glEnd */

/* An attribute function's name, parsed; false if it isn't one */
static bool parse_attrib(const char *name, struct attrib *a)
{
	static const struct {
		const char *prefix;
		uint8_t kind, count;
	} kinds[] = {
		{ "glVertex", A_VERTEX, 0 }, { "glColor", A_COLOR, 0 }, { "glSecondaryColor", A_SECONDARY, 0 },
		{ "glNormal", A_NORMAL, 0 }, { "glTexCoord", A_TEXCOORD, 0 }, { "glMultiTexCoord", A_MULTITEX, 0 },
		{ "glRasterPos", A_RASTERPOS, 0 }, { "glIndex", A_INDEX, 1 }, { "glEdgeFlag", A_EDGEFLAG, 1 },
		{ "glFogCoord", A_FOGCOORD, 1 }, { "glEvalCoord", A_EVALCOORD, 0 },
	};
	static const struct {
		const char *suffix;
		char type;
	} types[] = { { "ub", 'B' }, { "us", 'S' }, { "ui", 'I' }, { "b", 'b' }, { "s", 's' }, { "i", 'i' },
		      { "f", 'f' }, { "d", 'd' } };
	for (unsigned k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
		size_t n = strlen(kinds[k].prefix);
		if (strncmp(name, kinds[k].prefix, n))
			continue;
		const char *p = name + n;
		struct attrib r = { kinds[k].kind, kinds[k].count, 0, false };
		if (*p >= '1' && *p <= '4')
			r.count = (uint8_t)(*p++ - '0');
		if (r.kind == A_EDGEFLAG)
			r.type = 'B';
		for (unsigned j = 0; j < sizeof types / sizeof types[0] && !r.type; j++)
			if (!strncmp(p, types[j].suffix, strlen(types[j].suffix))) {
				r.type = types[j].type;
				p += strlen(types[j].suffix);
			}
		if (*p == 'v') {
			r.array = true;
			p++;
		}
		if (r.type && r.count && (!*p || !strcmp(p, "ARB") || !strcmp(p, "EXT"))) {
			*a = r;
			return true;
		}
	}
	return false;
}

/* How many elements a function's array has (by its name), and their type; 0 if that isn't fixed */
static unsigned array_of(const char *name, char *type)
{
	struct attrib a;
	if (parse_attrib(name, &a) && a.array) {
		*type = a.type;
		return a.count;
	}
	static const char *const matrices[] = { "glLoadMatrix", "glMultMatrix", "glLoadTransposeMatrix",
						"glMultTransposeMatrix" };
	for (unsigned k = 0; k < sizeof matrices / sizeof matrices[0]; k++) {
		size_t n = strlen(matrices[k]);
		const char *p = name + n;
		if (!strncmp(name, matrices[k], n) && (*p == 'f' || *p == 'd') && (!p[1] || !strcmp(p + 1, "ARB"))) {
			*type = *p;
			return 16;
		}
	}
	return 0;
}

/* What's recorded, what sets attributes, what may come between glBegin and glEnd */
static void make_tables(void)
{
	static const char *const not_recorded[] = { "glFlush", "glFinish", "glArrayElement", "glDrawArrays",
						    "glDrawElements", "glDrawRangeElements", "glLockArraysEXT",
						    "glUnlockArraysEXT" };
	static bool made;
	if (made)
		return;
	made = true;
	for (unsigned fn = 0; fn < GL_FUNCTIONS; fn++) {
		const struct function *f = &library_functions[fn];
		parse_attrib(f->name, &attribs[fn]);
		inside_ok[fn] = (attribs[fn].kind && attribs[fn].kind != A_RASTERPOS) || fn == F_glEnd ||
				!strncmp(f->name, "glMaterial", 10) || !strncmp(f->name, "glEvalPoint", 11) ||
				fn == F_glArrayElement || fn == F_glCallList || fn == F_glCallLists;
		bool skip = f->result[0] != 'v';
		for (unsigned k = 0; k < sizeof not_recorded / sizeof not_recorded[0]; k++)
			skip |= !strcmp(f->name, not_recorded[k]);
		struct recorded r;
		memset(&r, 0, sizeof r);
		unsigned size = 4, fprs = 0, n = 0;
		for (const char *p = f->args; *p && !skip; p++) {
			char type;
			unsigned count = *p == 'p' && !p[1] ? array_of(f->name, &type) : 0;
			if (*p == 'p' && (!count || n + count >= sizeof r.args)) {
				skip = true;
			} else if (*p == 'p') {
				r.count = (uint8_t)count;
				memset(r.args + n, type, count);
				n += count;
				size += count * (type == 'd' ? 8 : 4);
			} else {
				fprs += *p == 'f' || *p == 'd';
				r.args[n++] = *p;
				size += *p == 'd' ? 8 : 4;
			}
		}
		if (skip || fprs > 13 || size > RECORD_MAX)
			continue;
		r.size = (uint8_t)size;
		recorded[fn] = r;
	}
}

unsigned opengl_recorded(unsigned fn, const char **args)
{
	make_tables();
	if (fn >= GL_FUNCTIONS || !recorded[fn].size)
		return 0;
	*args = recorded[fn].args;
	return recorded[fn].size;
}

/* PowerPC instructions, for record_code() */
enum { OP_ADDI = 14, OP_ADDIS = 15, OP_ORI = 24, OP_LWZ = 32, OP_LBZ = 34, OP_STW = 36, OP_LHZ = 40, OP_STFS = 52,
       OP_STFD = 54 };
#define INSN_CMPLW(ra, rb) (31u << 26 | (ra) << 16 | (rb) << 11 | 32u << 1)     /* (cr0) */
#define INSN_BGT 0x41810000u                    /* bc 12,1 (cr0 gt) */
#define INSN_BNE 0x40820000u                    /* bc 4,2 (cr0 eq clear) */
#define INSN_BLR 0x4E800020u
#define INSN_B(off) (0x48000000u | ((uint32_t)(off) & 0x03FFFFFCu))

static uint32_t d_form(unsigned op, unsigned rt, unsigned ra, int32_t d)
{
	return (uint32_t)op << 26 | rt << 21 | ra << 16 | ((uint32_t)d & 0xFFFFu);
}

/*
 * The code recording a call to gl function fn (with table, as a context's
 * dispatch table calls it: the context first, which must be the current
 * one), `at` bytes into the code section, with the library's own data
 * `data` bytes into the data section. Returns its size in words (written
 * up to `room`).
 *
 *        [lwz r12,CURRENT(r2) ; cmplw r3,r12 ; bne host]  (table)
 *        lwz r11,RECORD(r2)              where the record goes
 *        lwz r12,RECORD_END(r2)
 *        addi r0,r11,size                ... and the next one
 *        cmplw r0,r12
 *        bgt host                        (no room)
 *        stw r0,RECORD(r2)
 *        lis r0,fn ; ori r0,r0,size ; stw r0,0(r11)
 *        stw r3,4(r11) ; stfs f1,8(r11) ...      the arguments
 *        blr
 * host:  b stub                          the entry's host call (li r11,entry ; b common)
 */
static size_t record_code(uint32_t *w, size_t room, uint32_t at, unsigned fn, bool table, uint32_t data)
{
	const struct recorded *r = &recorded[fn];
	size_t n = 0, branch[2], branches = 0;
#define EMIT(insn) \
	do { \
		if (n < room) \
			w[n] = (insn); \
		n++; \
	} while (0)
	const int32_t rec = (int32_t)(data + D_RECORD);
	if (table) {
		EMIT(d_form(OP_LWZ, 12, 2, (int32_t)(data + D_CURRENT)));
		EMIT(INSN_CMPLW(3u, 12u));
		branch[branches++] = n;
		EMIT(INSN_BNE);
	}
	EMIT(d_form(OP_LWZ, 11, 2, rec));
	EMIT(d_form(OP_LWZ, 12, 2, rec + 4));
	EMIT(d_form(OP_ADDI, 0, 11, r->size));
	EMIT(INSN_CMPLW(0u, 12u));
	branch[branches++] = n;
	EMIT(INSN_BGT);
	EMIT(d_form(OP_STW, 0, 2, rec));
	EMIT(d_form(OP_ADDIS, 0, 0, (int32_t)fn));
	EMIT(d_form(OP_ORI, 0, 0, r->size));
	EMIT(d_form(OP_STW, 0, 11, 0));
	unsigned word = table, fpr = 0;
	int32_t off = 4;
	for (const char *p = library_functions[fn].args; *p; p++) {
		switch (*p) {
		case 'f':
			EMIT(d_form(OP_STFS, 1 + fpr++, 11, off));
			word++;
			off += 4;
			break;
		case 'd':
			EMIT(d_form(OP_STFD, 1 + fpr++, 11, off));
			word += 2;
			off += 8;
			break;
		case 'p': {                     /* (the array: its elements copied) */
			unsigned base = 3 + word;
			if (word >= 8) {
				EMIT(d_form(OP_LWZ, 12, 1, 24 + 4 * (int32_t)word));
				base = 12;
			}
			word++;
			const char type = r->args[strlen(r->args) - 1];
			for (int32_t e = 0; e < r->count; e++) {
				switch (type) {
				case 'b': case 'B': EMIT(d_form(OP_LBZ, 0, base, e)); break;
				case 's': case 'S': EMIT(d_form(OP_LHZ, 0, base, 2 * e)); break;
				case 'd':
					EMIT(d_form(OP_LWZ, 0, base, 8 * e));
					EMIT(d_form(OP_STW, 0, 11, off));
					off += 4;
					EMIT(d_form(OP_LWZ, 0, base, 8 * e + 4));
					break;
				default: EMIT(d_form(OP_LWZ, 0, base, 4 * e)); break;
				}
				EMIT(d_form(OP_STW, 0, 11, off));
				off += 4;
			}
			break;
		}
		default:
			if (word < 8) {
				EMIT(d_form(OP_STW, 3 + word, 11, off));
			} else {
				EMIT(d_form(OP_LWZ, 0, 1, 24 + 4 * (int32_t)word));
				EMIT(d_form(OP_STW, 0, 11, off));
			}
			word++;
			off += 4;
			break;
		}
	}
	EMIT(INSN_BLR);
	const size_t host = n;
	const uint32_t stub = NDRV_STUB_SIZE * (table ? E_TABLE + fn : fn);
	EMIT(INSN_B((int32_t)stub - (int32_t)(at + 4 * (uint32_t)host)));
	for (size_t k = 0; k < branches; k++)
		if (branch[k] < room)
			w[branch[k]] |= (uint32_t)(4 * (host - branch[k])) & 0xFFFCu;
#undef EMIT
	return n;
}

size_t opengl_build_library(uint8_t *out, size_t size, struct ndrv_layout *layout)
{
	static struct ndrv_export exports[F_LIBRARY + 1];
	static uint32_t code[CODE_WORDS], entry_code[E_LIBRARY];
	make_data();
	make_tables();
	for (unsigned k = 0; k < F_LIBRARY; k++)
		exports[k] = (struct ndrv_export){ .name = library_functions[k].name, .entry = k };
	exports[F_LIBRARY] = (struct ndrv_export){ .name = "glc_cc", .data = true, .offset = D_CURRENT };
	struct ndrv_spec spec = {
		.kind = FRAG_LIBRARY,
		.entries = E_LIBRARY,
		.init_entry = E_INIT,
		.term_entry = E_TERM,
		.exports = exports,
		.export_count = F_LIBRARY + 1,
		.hostcall = HOSTCALL_TOOLS_GL,
		.extra = lib.data,
		.extra_len = lib.len,
		.zero_len = lib.end - (uint32_t)lib.len,
		.current_version = 1,           /* (as Apple's: see docs/opengl-plan.md) */
	};
	/* the layout first (the data's doesn't depend on the code), then the recording code (see above) */
	if (!ndrv_build(&spec, out, size, layout))
		return 0;
	const uint32_t first = ndrv_code_offset(E_LIBRARY);
	size_t n = 0;
	for (unsigned k = 0; k < E_LIBRARY; k++)
		entry_code[k] = NDRV_STUB;
	for (unsigned table = 0; table < 2; table++)
		for (unsigned fn = 0; fn < GL_FUNCTIONS; fn++) {
			if (!recorded[fn].size)
				continue;
			uint32_t at = first + 4 * (uint32_t)n;
			entry_code[table ? E_TABLE + fn : fn] = at;
			n += record_code(code + n, n < CODE_WORDS ? CODE_WORDS - n : 0, at, fn, table, layout->extra);
		}
	if (n > CODE_WORDS)
		return 0;
	spec.code = code;
	spec.code_words = n;
	spec.entry_code = entry_code;
	return ndrv_build(&spec, out, size, layout);
}

/* OpenGLMemory's allocations are Memory Manager blocks */
enum { MI_NEW_PTR, MI_NEW_PTR_CLEAR, MI_DISPOSE_PTR, MI_GET_PTR_SIZE, MI_COUNT };
static const struct ndrv_import memory_imports[MI_COUNT] = {
	[MI_NEW_PTR] = { "InterfaceLib", "NewPtr" },
	[MI_NEW_PTR_CLEAR] = { "InterfaceLib", "NewPtrClear" },
	[MI_DISPOSE_PTR] = { "InterfaceLib", "DisposePtr" },
	[MI_GET_PTR_SIZE] = { "InterfaceLib", "GetPtrSize" },
};

size_t opengl_build_memory(uint8_t *out, size_t size, struct ndrv_layout *layout)
{
	static struct ndrv_export exports[M_LIBRARY];
	for (unsigned k = 0; k < M_LIBRARY; k++)
		exports[k] = (struct ndrv_export){ .name = memory_functions[k].name, .entry = k };
	const struct ndrv_spec spec = {
		.kind = FRAG_LIBRARY, .entries = M_LIBRARY, .init_entry = -1, .exports = exports,
		.export_count = M_LIBRARY, .hostcall = HOSTCALL_TOOLS_GLM, .imports = memory_imports,
		.import_count = MI_COUNT, .current_version = 1,
	};
	return ndrv_build(&spec, out, size, layout);
}

size_t opengl_build_utility(uint8_t *out, size_t size, struct ndrv_layout *layout)
{
	static struct ndrv_export exports[U_LIBRARY];
	for (unsigned k = 0; k < U_LIBRARY; k++)
		exports[k] = (struct ndrv_export){ .name = utility_functions[k].name, .entry = k };
	const struct ndrv_spec spec = {
		.kind = FRAG_LIBRARY, .entries = U_LIBRARY, .init_entry = -1, .exports = exports,
		.export_count = U_LIBRARY, .hostcall = HOSTCALL_TOOLS_GLU, .current_version = 1,
	};
	return ndrv_build(&spec, out, size, layout);
}

int opengl_entry(const char *name)
{
	for (unsigned k = 0; k < F_LIBRARY; k++)
		if (!strcmp(library_functions[k].name, name))
			return (int)k;
	return -1;
}

void opengl_init(struct opengl *o, struct mem *mem)
{
	static uint8_t scratch[OPENGL_LIBRARY_MAX];
	memset(o, 0, sizeof *o);
	o->mem = mem;
	opengl_build_library(scratch, sizeof scratch, &o->library);
	opengl_build_memory(scratch, sizeof scratch, &o->memory);
	opengl_build_utility(scratch, sizeof scratch, &o->utility);
}

/* ---- guest memory ---------------------------------------------------------------- */

static bool readable(const struct mem *m, uint32_t addr, uint32_t len)
{
	if (!len)
		return true;
	if (addr + len - 1 < addr)
		return false;
	for (uint32_t p = addr >> MEM_PAGE_SHIFT; p <= (addr + len - 1) >> MEM_PAGE_SHIFT; p++)
		if (!(m->pageflags[p] & MEM_R))
			return false;
	return true;
}

/* Writable, with any translated code there dropped (the host is about to write) */
static bool writable(struct mem *m, uint32_t addr, uint32_t len)
{
	if (!len)
		return true;
	if (addr + len - 1 < addr)
		return false;
	for (uint32_t p = addr >> MEM_PAGE_SHIFT; p <= (addr + len - 1) >> MEM_PAGE_SHIFT; p++) {
		if (!(m->pageflags[p] & MEM_W))
			return false;
		if (m->pageflags[p] & MEM_CODE)
			mem_invalidate(m, p << MEM_PAGE_SHIFT, MEM_PAGE_SIZE);
	}
	return true;
}

static void put_word(struct mem *m, uint32_t addr, uint32_t v)
{
	if (addr && writable(m, addr, 4))
		mem_w32(m, addr, v);
}

/* ---- instances, contexts ------------------------------------------------------------ */

static struct opengl_instance *instance_of(struct opengl *o, uint32_t toc, bool make)
{
	struct opengl_instance *free_one = NULL;
	for (unsigned k = 0; k < OPENGL_INSTANCES; k++) {
		if (o->inst[k].toc == toc)
			return &o->inst[k];
		if (!o->inst[k].toc && !free_one)
			free_one = &o->inst[k];
	}
	if (!make || !free_one)
		return NULL;
	memset(free_one, 0, sizeof *free_one);
	free_one->toc = toc;
	return free_one;
}

static uint32_t data_of(const struct opengl *o, const struct opengl_instance *in)
{
	return in->toc + o->library.extra;
}

static void free_context(struct opengl *o, struct opengl_context *x)
{
	if (x->has_surface)
		surface_free(&x->surface);
	glf_detach(&x->st, o->gpu);
	memset(x, 0, sizeof *x);
}

/* Textures retired since, freed if no context has drawing recorded that could use them */
static void free_retired(struct opengl *o)
{
	for (unsigned i = 0; i < OPENGL_INSTANCES; i++)
		for (unsigned k = 0; k < OPENGL_CONTEXTS; k++)
			if (o->inst[i].ctx[k].has_surface && surface_busy(&o->inst[i].ctx[k].surface))
				return;
	for (unsigned i = 0; i < OPENGL_INSTANCES; i++)
		for (unsigned k = 0; k < OPENGL_CONTEXTS; k++)
			if (o->inst[i].ctx[k].used && o->inst[i].ctx[k].st.names)
				glf_free_retired(o->inst[i].ctx[k].st.names, o->gpu);
}

static void free_instance(struct opengl *o, struct opengl_instance *in)
{
	for (unsigned k = 0; k < OPENGL_CONTEXTS; k++)
		if (in->ctx[k].used)
			free_context(o, &in->ctx[k]);
	memset(in, 0, sizeof *in);
}

void opengl_free(struct opengl *o)
{
	for (unsigned k = 0; k < OPENGL_INSTANCES; k++)
		if (o->inst[k].toc)
			free_instance(o, &o->inst[k]);
}

/* The context whose AGLContext this is (in this instance), or NULL */
static struct opengl_context *context_at(struct opengl *o, struct opengl_instance *in, uint32_t guest)
{
	if (!in || !guest)
		return NULL;
	uint32_t first = data_of(o, in) + lib.contexts;
	uint32_t k = (guest - first) / CONTEXT_SIZE;
	if (guest < first || (guest - first) % CONTEXT_SIZE || k >= OPENGL_CONTEXTS || !in->ctx[k].used)
		return NULL;
	return &in->ctx[k];
}

/* The GPU, if contexts are to be drawn by it: asked once, when the first is made */
static struct gpu *gpu_of(struct opengl *o)
{
	if (o->gpu && !o->gpu_asked) {
		const char *why = NULL;
		o->gpu_asked = true;
		if (!gpu_ready(o->gpu, &why) || gpu_failed(o->gpu)) {
			printf("opengl: drawing in software (%s)\n", why ? why : "OpenGL failed");
			o->gpu = NULL;
		} else {
			printf("opengl: drawing with %s\n", gpu_renderer(o->gpu));
		}
		fflush(stdout);
	}
	return o->gpu;
}

/* The GPU failed: every context is drawn in software from now on */
static void lose_gpu(struct opengl *o)
{
	if (!o->gpu)
		return;
	printf("opengl: OpenGL failed; drawing in software from now on\n");
	fflush(stdout);
	for (unsigned i = 0; i < OPENGL_INSTANCES; i++)
		for (unsigned k = 0; k < OPENGL_CONTEXTS; k++)
			if (o->inst[i].ctx[k].has_surface)
				surface_lose_gpu(&o->inst[i].ctx[k].surface);
	o->gpu = NULL;
}

/* ---- drawables ---------------------------------------------------------------------- */

enum { DRAWABLE_NONE, DRAWABLE_PORT, DRAWABLE_OFFSCREEN };

/* A color port's pixmap: base, row bytes, bounds (top, left, bottom, right), bits a pixel; false if none */
static bool port_pixmap(const struct mem *m, uint32_t port, uint32_t *base, uint32_t *row_bytes, int bounds[4],
			unsigned *bits)
{
	if (!readable(m, port, 32) || (mem_r16(m, port + 6) & 0xC000) != 0xC000)
		return false;
	uint32_t pmh = mem_r32(m, port + 2);
	if (!readable(m, pmh, 4))
		return false;
	uint32_t pm = mem_r32(m, pmh);
	if (!readable(m, pm, 36))
		return false;
	*base = mem_r32(m, pm);
	*row_bytes = mem_r16(m, pm + 4) & 0x3FFF;
	for (int k = 0; k < 4; k++)
		bounds[k] = (int16_t)mem_r16(m, pm + 6 + 2 * k);
	*bits = mem_r16(m, pm + 32);
	return true;
}

/* A port's portRect: top, left, bottom, right */
static bool port_rect(const struct mem *m, uint32_t port, int r[4])
{
	if (!readable(m, port, 24))
		return false;
	for (int k = 0; k < 4; k++)
		r[k] = (int16_t)mem_r16(m, port + 16 + 2 * k);
	return r[2] > r[0] && r[3] > r[1];
}

/*
 * The drawable's size and where it is, from the port (or the buffer
 * rectangle, which counts from the bottom left of the port), and buffers of
 * that size; false if there are none
 */
static bool fit_drawable(struct opengl *o, struct opengl_context *x)
{
	int w = x->width, h = x->height, ox = x->origin_x, oy = x->origin_y;
	if (x->kind == DRAWABLE_PORT) {
		int r[4];
		if (!port_rect(o->mem, x->port, r))
			return false;
		w = r[3] - r[1];
		h = r[2] - r[0];
		ox = r[1];
		oy = r[0];
		if (x->enabled & 1u << OPTION_BUFFER_RECT && x->buffer_rect[2] > 0 && x->buffer_rect[3] > 0) {
			ox = r[1] + x->buffer_rect[0];
			oy = r[2] - x->buffer_rect[1] - x->buffer_rect[3];
			w = x->buffer_rect[2];
			h = x->buffer_rect[3];
		}
	}
	if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
		return false;
	x->origin_x = ox;
	x->origin_y = oy;
	if (x->has_surface && w == x->width && h == x->height)
		return true;
	if (x->has_surface)
		surface_free(&x->surface);
	x->has_surface = surface_init(&x->surface, w, h, gpu_of(o));
	if (!x->has_surface)
		return false;
	x->width = w;
	x->height = h;
	glf_changed(&x->st);
	if (!x->attached) {             /* (as OpenGL says: the drawable's size, when first attached) */
		x->attached = true;
		glf_viewport(&x->st, 0, 0, w, h);
		glf_scissor(&x->st, 0, 0, w, h);
	}
	return true;
}

/* The frame drawn so far, into the drawable (but for the swap rectangle's outside) */
static void show(struct opengl *o, struct opengl_context *x)
{
	if (!x->has_surface || x->kind == DRAWABLE_NONE)
		return;
	struct timespec t0, t1;
	if (o->fps)
		clock_gettime(CLOCK_MONOTONIC, &t0);
	bool gpu = x->surface.gpu != NULL;
	if (!surface_finish(&x->surface))
		lose_gpu(o);
	else if (x->surface.gpu)
		o->gpu_frames++;
	free_retired(o);
	if (o->fps) {                   /* (SYSHM_GL_FPS: the frame rate, every second of host time) */
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double now = (double)t1.tv_sec + 1e-9 * (double)t1.tv_nsec;
		o->fps_drawing += now - ((double)t0.tv_sec + 1e-9 * (double)t0.tv_nsec);
		o->fps_frames++;
		if (!o->fps_since) {
			o->fps_since = now;
		} else if (now - o->fps_since >= 1) {
			double span = now - o->fps_since;
			printf("opengl: %.1f frames/s %s, waiting for drawing %.0f%% of the time\n", o->fps_frames / span,
			       gpu ? "on the GPU" : "in software", 100 * o->fps_drawing / span);
			fflush(stdout);
			o->fps_since = now;
			o->fps_frames = 0;
			o->fps_drawing = 0;
		}
	}
	x->frames++;
	o->frames++;
	int l = 0, t = 0, r = x->width, b = x->height;
	if (x->enabled & 1u << OPTION_SWAP_RECT) {
		l = x->swap_rect[0];
		r = l + x->swap_rect[2];
		b = x->height - x->swap_rect[1];
		t = b - x->swap_rect[3];
	}
	struct surface_dest d;
	memset(&d, 0, sizeof d);
	if (x->kind == DRAWABLE_OFFSCREEN) {
		d.base = x->base;
		d.row_bytes = x->row_bytes;
		d.bits = x->pixel_size;
		d.right = x->width;
		d.bottom = x->height;
		d.alpha = true;
	} else {
		int bounds[4];
		if (!port_pixmap(o->mem, x->port, &d.base, &d.row_bytes, bounds, &d.bits))
			return;
		d.right = bounds[3] - bounds[1];
		d.bottom = bounds[2] - bounds[0];
		d.x = x->origin_x - bounds[1];
		d.y = x->origin_y - bounds[0];
		d.region[0] = mem_r32(o->mem, x->port + 24);    /* visRgn, clipRgn */
		d.region[1] = mem_r32(o->mem, x->port + 28);
		d.region_x = x->origin_x;
		d.region_y = x->origin_y;
	}
	surface_present(&x->surface, o->mem, &d, l, t, r, b);
}

/* ---- arguments ------------------------------------------------------------------------ */

union arg {
	uint32_t u;
	int32_t i;
	float f;
	double d;
};

/* A call's arguments by its function's signature (after `skip` words) */
static unsigned get_args(const struct ndrv_call *c, const char *sig, unsigned skip, union arg *a)
{
	struct ndrv_args it = ndrv_args(c);
	for (unsigned k = 0; k < skip; k++)
		ndrv_next_word(&it);
	unsigned n = 0;
	for (; sig[n]; n++) {
		uint32_t w;
		uint64_t q;
		switch (sig[n]) {
		case 'f':
			w = ndrv_next_float(&it);
			memcpy(&a[n].f, &w, 4);
			break;
		case 'd':
			q = ndrv_next_double(&it);
			memcpy(&a[n].d, &q, 8);
			break;
		case 'b': a[n].i = (int8_t)ndrv_next_word(&it); break;
		case 'B': a[n].u = ndrv_next_word(&it) & 0xFF; break;
		case 's': a[n].i = (int16_t)ndrv_next_word(&it); break;
		case 'S': a[n].u = ndrv_next_word(&it) & 0xFFFF; break;
		default: a[n].u = ndrv_next_word(&it); break;
		}
	}
	return n;
}

static void log_call(const char *lib_name, const char *name, const char *types, const union arg *a, unsigned n,
		     const char *what)
{
	printf("%s: %s(", lib_name, name);
	for (unsigned k = 0; k < n; k++) {
		const char *sep = k ? ", " : "";
		switch (types[k]) {
		case 'f': printf("%s%g", sep, (double)a[k].f); break;
		case 'd': printf("%s%g", sep, a[k].d); break;
		case 'b': case 's': case 'i': printf("%s%d", sep, a[k].i); break;
		default: printf("%s0x%X", sep, a[k].u); break;
		}
	}
	printf(")%s\n", what);
	fflush(stdout);
}

/* ---- GL ------------------------------------------------------------------------------- */

static void not_yet(struct opengl *o, unsigned fn)
{
	if (!o->unimplemented[fn]) {
		o->unimplemented[fn] = 1;
		printf("opengl: %s isn't done yet\n", library_functions[fn].name);
		fflush(stdout);
	}
}

static struct gl_target target_of(struct opengl_context *x)
{
	return (struct gl_target){ x->has_surface ? &x->surface : NULL, x->width, x->height, x->depth_bits,
				   x->pixel_size == 32 ? 8 : 0, x->stencil_bits };
}

static float clampf(float v)
{
	return v > 0 ? (v < 1 ? v : 1) : 0;
}

/* An attribute's value given as type t: colors' and normals' integers mapped to 0..1 (or -1..1 if signed) */
static float attrib_value(const union arg *a, char t, bool normalized)
{
	switch (t) {
	case 'f': return a->f;
	case 'd': return (float)a->d;
	case 'b': return normalized ? (float)((2.0 * a->i + 1) / 255) : (float)a->i;
	case 'B': return normalized ? (float)(a->u / 255.0) : (float)a->u;
	case 's': return normalized ? (float)((2.0 * a->i + 1) / 65535) : (float)a->i;
	case 'S': return normalized ? (float)(a->u / 65535.0) : (float)a->u;
	case 'i': return normalized ? (float)((2.0 * a->i + 1) / 4294967295.0) : (float)a->i;
	default: return normalized ? (float)(a->u / 4294967295.0) : (float)a->u;
	}
}

/* glVertex, glColor and the rest (attribs[fn]) */
static void set_attrib(struct opengl *o, struct opengl_context *x, unsigned fn, const union arg *a)
{
	const struct attrib *at = &attribs[fn];
	struct gl_state *st = &x->st;
	const unsigned first = at->kind == A_MULTITEX;          /* (the texture unit first) */
	const bool normalized = at->kind == A_COLOR || at->kind == A_SECONDARY || at->kind == A_NORMAL;
	float v[4] = { 0, 0, 0, 1 };
	for (unsigned k = 0; k < at->count; k++)
		v[k] = attrib_value(&a[first + k], at->type, normalized);
	switch (at->kind) {
	case A_VERTEX: {
		struct gl_target t = target_of(x);
		glf_vertex(st, &t, v);
		break;
	}
	case A_COLOR: memcpy(st->color, v, sizeof v); break;
	case A_SECONDARY: memcpy(st->secondary_color, v, 3 * sizeof *v); break;
	case A_NORMAL: memcpy(st->normal, v, 3 * sizeof *v); break;
	case A_TEXCOORD: memcpy(st->tex[0], v, sizeof v); break;
	case A_MULTITEX:
		if (a[0].u - GL_TEXTURE0_ARB < GLF_TEXTURE_UNITS)
			memcpy(st->tex[a[0].u - GL_TEXTURE0_ARB], v, sizeof v);
		else
			glf_error(st, GL_INVALID_ENUM);
		break;
	case A_EDGEFLAG: st->edge_flag = a[0].u != 0; break;
	case A_FOGCOORD: st->fog_coord = v[0]; break;
	case A_INDEX: break;                    /* (there's no color index mode) */
	default: not_yet(o, fn); break;         /* (raster positions, evaluators) */
	}
}

/* glLoadMatrix and its kin's argument (as glLoadMatrixf's) */
static void matrix_arg(unsigned fn, const union arg *a, float m[16])
{
	bool doubles = recorded[fn].args[0] == 'd';
	bool transpose = fn == F_glLoadTransposeMatrixfARB || fn == F_glLoadTransposeMatrixdARB ||
			 fn == F_glMultTransposeMatrixfARB || fn == F_glMultTransposeMatrixdARB;
	for (unsigned k = 0; k < 16; k++) {
		unsigned from = transpose ? (k & 3) * 4 + k / 4 : k;
		m[k] = doubles ? (float)a[from].d : a[from].f;
	}
}

/* State glGetIntegerv gives as fractions of the integers' range (colors, normals, depths) */
static bool scaled(uint32_t pname)
{
	return pname == 0x0B00 || pname == 0x0B02 || pname == 0x0C22 || pname == 0x8005 || pname == 0x0BC2 ||
	       pname == 0x0B73 || pname == 0x0B70 || pname == 0x8459;
}

/* A state's values (glGetDoublev's); 0 for an enum it doesn't know */
static unsigned gl_get(struct opengl_context *x, uint32_t pname, double *v)
{
	unsigned n = glf_get(&x->st, pname, v);
	if (n)
		return n;
	bool deep = x->pixel_size == 32;
	switch (pname) {
	case GL_DOUBLEBUFFER: v[0] = (x->format & PF_DOUBLE) != 0; return 1;
	case GL_MAX_TEXTURE_SIZE: v[0] = GLF_MAX_TEXTURE_SIZE; return 1;
	case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: v[0] = deep ? 8 : 5; return 1;
	case GL_ALPHA_BITS: v[0] = deep ? 8 : 0; return 1;
	case GL_DEPTH_BITS: v[0] = x->depth_bits; return 1;
	case GL_STENCIL_BITS: v[0] = x->stencil_bits; return 1;
	default: return 0;
	}
}

/* glGet*v's values into the program's memory, as fn's type (glGetBooleanv's... or glGetTexParameteriv's...) */
static void put_values(struct mem *m, uint32_t at, char type, uint32_t pname, const double *v, unsigned n)
{
	unsigned size = type == 'B' ? 1 : type == 'd' ? 8 : 4;
	if (!writable(m, at, n * size))
		return;
	for (unsigned k = 0; k < n; k++, at += size) {
		if (type == 'B') {
			mem_w8(m, at, v[k] != 0);
		} else if (type == 'i') {
			double d = scaled(pname) ? (v[k] < -1 ? -1 : v[k] > 1 ? 1 : v[k]) * 2147483647.0 : floor(v[k] + 0.5);
			d = d < -2147483648.0 ? -2147483648.0 : d > 2147483647.0 ? 2147483647.0 : d;
			mem_w32(m, at, (uint32_t)(int32_t)d);
		} else if (type == 'f') {
			float f = (float)v[k];
			uint32_t b;
			memcpy(&b, &f, 4);
			mem_w32(m, at, b);
		} else {
			uint64_t b;
			memcpy(&b, &v[k], 8);
			mem_w64(m, at, b);
		}
	}
}

/* An integer as a color component (glTexEnviv's, glTexParameteriv's): -1..1 */
static float int_color(int32_t c)
{
	return (float)((2.0 * c + 1) / 4294967295.0);
}

/* glTexParameterv's and glTexEnvv's values (4 for colors, else 1), as floats; false if they can't be read */
static bool vector_arg(struct mem *m, uint32_t at, uint32_t pname, bool ints, float *v)
{
	bool color = pname == 0x1004 || pname == GL_TEXTURE_ENV_COLOR;     /* (GL_TEXTURE_BORDER_COLOR) */
	unsigned n = color ? 4 : 1;
	if (!readable(m, at, 4 * n))
		return false;
	for (unsigned k = 0; k < n; k++) {
		uint32_t w = mem_r32(m, at + 4 * k);
		if (!ints)
			memcpy(&v[k], &w, 4);
		else
			v[k] = color ? int_color((int32_t)w) : (float)(int32_t)w;
	}
	return true;
}

/* ---- lighting, fog, texture coordinate generation: their vectors ---- */

/* Which glLight, glMaterial, glLightModel and glFog parameters are colors (integers given as -1..1) */
static bool color_pname(uint32_t pname)
{
	return pname == GL_AMBIENT || pname == GL_DIFFUSE || pname == GL_SPECULAR || pname == GL_EMISSION ||
	       pname == GL_AMBIENT_AND_DIFFUSE || pname == GL_LIGHT_MODEL_AMBIENT || pname == GL_FOG_COLOR;
}

/* How many values a parameter of glLight..., glMaterial..., glLightModel..., glFog..., glTexGen... takes */
static unsigned pname_values(uint32_t pname)
{
	switch (pname) {
	case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_POSITION: case GL_EMISSION:
	case GL_AMBIENT_AND_DIFFUSE: case GL_LIGHT_MODEL_AMBIENT: case GL_FOG_COLOR:
	case GL_OBJECT_PLANE: case GL_EYE_PLANE:
		return 4;
	case GL_SPOT_DIRECTION: case GL_COLOR_INDEXES: return 3;
	default: return 1;
	}
}

/* A parameter vector from the program's memory as floats (of type 'f', 'i' or 'd'); false if it can't be read */
static bool param_vector(struct mem *m, uint32_t at, char type, uint32_t pname, unsigned n, float *v)
{
	unsigned size = type == 'd' ? 8 : 4;
	if (!readable(m, at, n * size))
		return false;
	for (unsigned k = 0; k < n; k++) {
		if (type == 'd') {
			uint64_t b = mem_r64(m, at + 8 * k);
			double d;
			memcpy(&d, &b, 8);
			v[k] = (float)d;
		} else {
			uint32_t w = mem_r32(m, at + 4 * k);
			if (type == 'f')
				memcpy(&v[k], &w, 4);
			else
				v[k] = color_pname(pname) ? int_color((int32_t)w) : (float)(int32_t)w;
		}
	}
	return true;
}

/* glGetLight..., glGetMaterial..., glGetTexGen..., glGetClipPlane's values into the program's memory */
static void put_params(struct mem *m, uint32_t at, char type, uint32_t pname, const double *v, unsigned n)
{
	put_values(m, at, type, type == 'i' && color_pname(pname) ? 0x0B00 : 0, v, n);
}

/* ---- client arrays ---- */

/* Each enabled array's elements [first, first + n) on the host, where the program's memory has them */
static void arrays_at(struct opengl *o, struct gl_state *st, uint32_t first, uint32_t n)
{
	for (unsigned k = 0; k < GLF_ARRAYS; k++) {
		struct gl_array *a = &st->arrays[k];
		a->data = NULL;
		a->first = first;
		a->count = 0;
		if (!a->enabled || !n)
			continue;
		uint64_t at = (uint64_t)a->pointer + (uint64_t)first * glf_array_stride(a), bytes = glf_array_bytes(a, n);
		if (at + bytes <= 0x100000000ull && bytes <= (64u << 20) && readable(o->mem, (uint32_t)at, (uint32_t)bytes)) {
			a->data = mem_ptr(o->mem, (uint32_t)at);
			a->count = n;
		}
	}
}

/* glDrawElements' and glDrawRangeElements': the indices read, the arrays for the elements they name */
static void draw_elements(struct opengl *o, struct opengl_context *x, uint32_t mode, int32_t count, uint32_t type,
			  uint32_t indices)
{
	struct gl_state *st = &x->st;
	struct gl_target t = target_of(x);
	unsigned size = glf_index_size(type);
	if (!size || count <= 0 || (uint64_t)count * size > (64u << 20) ||
	    !readable(o->mem, indices, (uint32_t)count * size)) {
		glf_draw_elements(st, &t, mode, size ? count : 0, type, NULL);  /* (its errors) */
		return;
	}
	const uint8_t *p = mem_ptr(o->mem, indices);
	uint32_t lo = UINT32_MAX, hi = 0;
	for (int32_t k = 0; k < count; k++) {
		const uint8_t *q = p + (size_t)k * size;
		uint32_t i = size == 1 ? q[0] : size == 2 ? be16(q) : be32(q);
		lo = i < lo ? i : lo;
		hi = i > hi ? i : hi;
	}
	arrays_at(o, st, lo, hi - lo + 1);
	glf_draw_elements(st, &t, mode, count, type, p);
}

/* glTexImage1D and 2D, glTexSubImage1D and 2D: the image's bytes there to be read (or none given) */
static void tex_image(struct opengl *o, struct gl_state *st, bool sub, uint32_t target, int32_t level, int32_t internal,
		      int32_t x, int32_t y, int32_t w, int32_t h, int32_t border, uint32_t format, uint32_t type,
		      uint32_t pixels)
{
	size_t bytes = glf_image_bytes(&st->unpack, format, type, w, h);
	const uint8_t *p = NULL;
	if (pixels && bytes) {
		if (bytes > (64u << 20) || !readable(o->mem, pixels, (uint32_t)bytes))
			return;                 /* (the program would have crashed reading it) */
		p = mem_ptr(o->mem, pixels);
	}
	if (sub)
		glf_tex_sub_image(st, target, level, x, y, w, h, format, type, p);
	else
		glf_tex_image(st, target, level, internal, w, h, border, format, type, p);
}

/* glDeleteTextures' one: no longer bound in any context of its namespace */
static void delete_texture(struct opengl *o, struct opengl_context *x, uint32_t name)
{
	const struct gl_texture *t = glf_texture(&x->st, name);
	if (!t)
		return;
	for (unsigned i = 0; i < OPENGL_INSTANCES; i++)
		for (unsigned k = 0; k < OPENGL_CONTEXTS; k++)
			if (o->inst[i].ctx[k].used && o->inst[i].ctx[k].st.names == x->st.names)
				glf_unbind(&o->inst[i].ctx[k].st, t);
	glf_delete_texture(&x->st, name);
}

static uint32_t gl_call(struct opengl *o, struct opengl_context *x, unsigned fn, const union arg *a)
{
	struct gl_state *st = &x->st;
	struct mem *m = o->mem;
	if (st->inside && !inside_ok[fn]) {
		glf_error(st, GL_INVALID_OPERATION);
		return 0;
	}
	if (attribs[fn].kind) {
		set_attrib(o, x, fn, a);
		return 0;
	}
	struct gl_target t = target_of(x);
	float mat[16];
	switch (fn) {
	case F_glClearColor:
		for (int k = 0; k < 4; k++)
			st->clear_color[k] = clampf(a[k].f);
		return 0;
	case F_glClearDepth:
		st->clear_depth = a[0].d < 0 ? 0 : a[0].d > 1 ? 1 : a[0].d;
		return 0;
	case F_glClear: glf_clear(st, &t, a[0].u); return 0;
	case F_glViewport: glf_viewport(st, a[0].i, a[1].i, a[2].i, a[3].i); return 0;
	case F_glScissor: glf_scissor(st, a[0].i, a[1].i, a[2].i, a[3].i); return 0;
	case F_glDepthRange: glf_depth_range(st, a[0].d, a[1].d); return 0;
	case F_glEnable:
	case F_glDisable:
		if (!glf_enable(st, a[0].u, fn == F_glEnable))
			glf_error(st, GL_INVALID_ENUM);
		return 0;
	case F_glIsEnabled: {
		int on = glf_is_enabled(st, a[0].u);
		if (on < 0)
			glf_error(st, GL_INVALID_ENUM);
		return on > 0;
	}
	case F_glColorMask: glf_color_mask(st, a[0].u != 0, a[1].u != 0, a[2].u != 0, a[3].u != 0); return 0;
	case F_glDepthMask: glf_depth_mask(st, a[0].u != 0); return 0;
	case F_glDepthFunc: glf_depth_func(st, a[0].u); return 0;
	case F_glBlendFunc: glf_blend_func(st, a[0].u, a[1].u); return 0;
	case F_glBlendEquation: case F_glBlendEquationEXT: glf_blend_equation(st, a[0].u); return 0;
	case F_glBlendColor: case F_glBlendColorEXT: glf_blend_color(st, a[0].f, a[1].f, a[2].f, a[3].f); return 0;
	case F_glAlphaFunc: glf_alpha_func(st, a[0].u, a[1].f); return 0;
	case F_glCullFace: glf_cull_face(st, a[0].u); return 0;
	case F_glFrontFace: glf_front_face(st, a[0].u); return 0;
	case F_glShadeModel: glf_shade_model(st, a[0].u); return 0;
	case F_glPointSize: glf_point_size(st, a[0].f); return 0;
	case F_glLineWidth: glf_line_width(st, a[0].f); return 0;
	case F_glHint: return 0;                /* (hints taken: the same either way) */
	case F_glActiveTextureARB: glf_active_texture(st, a[0].u); return 0;
	case F_glMatrixMode: glf_matrix_mode(st, a[0].u); return 0;
	case F_glLoadIdentity: glf_identity(st); return 0;
	case F_glLoadMatrixf: case F_glLoadMatrixd: case F_glLoadTransposeMatrixfARB: case F_glLoadTransposeMatrixdARB:
		matrix_arg(fn, a, mat);
		glf_load(st, mat);
		return 0;
	case F_glMultMatrixf: case F_glMultMatrixd: case F_glMultTransposeMatrixfARB: case F_glMultTransposeMatrixdARB:
		matrix_arg(fn, a, mat);
		glf_mult(st, mat);
		return 0;
	case F_glTranslatef: glf_translate(st, a[0].f, a[1].f, a[2].f); return 0;
	case F_glTranslated: glf_translate(st, (float)a[0].d, (float)a[1].d, (float)a[2].d); return 0;
	case F_glScalef: glf_scale(st, a[0].f, a[1].f, a[2].f); return 0;
	case F_glScaled: glf_scale(st, (float)a[0].d, (float)a[1].d, (float)a[2].d); return 0;
	case F_glRotatef: glf_rotate(st, a[0].f, a[1].f, a[2].f, a[3].f); return 0;
	case F_glRotated: glf_rotate(st, (float)a[0].d, (float)a[1].d, (float)a[2].d, (float)a[3].d); return 0;
	case F_glFrustum: glf_frustum(st, a[0].d, a[1].d, a[2].d, a[3].d, a[4].d, a[5].d); return 0;
	case F_glOrtho: glf_ortho(st, a[0].d, a[1].d, a[2].d, a[3].d, a[4].d, a[5].d); return 0;
	case F_glPushMatrix: glf_push(st); return 0;
	case F_glPopMatrix: glf_pop(st); return 0;
	case F_glBegin: glf_begin(st, a[0].u); return 0;
	case F_glEnd: glf_end(st, &t); return 0;
	case F_glGetError: {
		uint32_t e = st->error;
		st->error = GL_NO_ERROR;
		return e;
	}
	case F_glGetString: {
		uint32_t base = data_of(o, x->inst);
		switch (a[0].u) {
		case GL_VENDOR: return base + lib.vendor;
		case GL_RENDERER: return base + lib.renderer;
		case GL_VERSION: return base + lib.version;
		case GL_EXTENSIONS: return base + lib.extensions;
		default:
			glf_error(st, GL_INVALID_ENUM);
			return 0;
		}
	}
	case F_glGetBooleanv:
	case F_glGetIntegerv:
	case F_glGetFloatv:
	case F_glGetDoublev: {
		double v[16];
		unsigned n = gl_get(x, a[0].u, v);
		if (!n) {
			glf_error(st, GL_INVALID_ENUM);
			return 0;
		}
		put_values(m, a[1].u, fn == F_glGetBooleanv ? 'B' : fn == F_glGetIntegerv ? 'i' :
				      fn == F_glGetFloatv ? 'f' : 'd', a[0].u, v, n);
		return 0;
	}
	case F_glFlush:
	case F_glFinish:
		if (!(x->format & PF_DOUBLE))
			show(o, x);
		return 0;
	/* textures */
	case F_glBindTexture: glf_bind_texture(st, a[0].u, a[1].u); return 0;
	case F_glGenTextures:
		if (a[0].i < 0) {
			glf_error(st, GL_INVALID_VALUE);
			return 0;
		}
		if (a[0].i > (1 << 20) || !writable(m, a[1].u, 4 * a[0].u))
			return 0;
		for (uint32_t k = 0; k < a[0].u; k++) {
			uint32_t name;
			glf_gen_textures(st, 1, &name);
			mem_w32(m, a[1].u + 4 * k, name);
		}
		return 0;
	case F_glDeleteTextures:
		if (a[0].i < 0) {
			glf_error(st, GL_INVALID_VALUE);
			return 0;
		}
		if (a[0].i > (1 << 20) || !readable(m, a[1].u, 4 * a[0].u))
			return 0;
		for (uint32_t k = 0; k < a[0].u; k++)
			delete_texture(o, x, mem_r32(m, a[1].u + 4 * k));
		return 0;
	case F_glIsTexture: return glf_is_texture(st, a[0].u);
	case F_glAreTexturesResident: return 1;         /* (all are: then the list isn't written) */
	case F_glPrioritizeTextures: return 0;
	case F_glTexImage1D:
		tex_image(o, st, false, a[0].u, a[1].i, a[2].i, 0, 0, a[3].i, 1, a[4].i, a[5].u, a[6].u, a[7].u);
		return 0;
	case F_glTexImage2D:
		tex_image(o, st, false, a[0].u, a[1].i, a[2].i, 0, 0, a[3].i, a[4].i, a[5].i, a[6].u, a[7].u, a[8].u);
		return 0;
	case F_glTexSubImage1D:
		tex_image(o, st, true, a[0].u, a[1].i, 0, a[2].i, 0, a[3].i, 1, 0, a[4].u, a[5].u, a[6].u);
		return 0;
	case F_glTexSubImage2D:
		tex_image(o, st, true, a[0].u, a[1].i, 0, a[2].i, a[3].i, a[4].i, a[5].i, 0, a[6].u, a[7].u, a[8].u);
		return 0;
	case F_glTexParameteri:
	case F_glTexParameterf:
	case F_glTexEnvi:
	case F_glTexEnvf: {
		float v[4] = { fn == F_glTexParameteri || fn == F_glTexEnvi ? (float)a[2].i : a[2].f, 0, 0, 0 };
		if (fn == F_glTexEnvi || fn == F_glTexEnvf)
			glf_tex_env(st, a[0].u, a[1].u, v);
		else
			glf_tex_parameter(st, a[0].u, a[1].u, v);
		return 0;
	}
	case F_glTexParameteriv:
	case F_glTexParameterfv:
	case F_glTexEnviv:
	case F_glTexEnvfv: {
		float v[4];
		bool env = fn == F_glTexEnviv || fn == F_glTexEnvfv;
		if (vector_arg(m, a[2].u, a[1].u, fn == F_glTexParameteriv || fn == F_glTexEnviv, v)) {
			if (env)
				glf_tex_env(st, a[0].u, a[1].u, v);
			else
				glf_tex_parameter(st, a[0].u, a[1].u, v);
		}
		return 0;
	}
	case F_glGetTexParameteriv:
	case F_glGetTexParameterfv:
	case F_glGetTexEnviv:
	case F_glGetTexEnvfv:
	case F_glGetTexLevelParameteriv:
	case F_glGetTexLevelParameterfv: {
		double v[4];
		bool level = fn == F_glGetTexLevelParameteriv || fn == F_glGetTexLevelParameterfv;
		unsigned n = level ? glf_get_tex_level_parameter(st, a[0].u, a[1].i, a[2].u, v) :
			     fn == F_glGetTexEnviv || fn == F_glGetTexEnvfv ? glf_get_tex_env(st, a[0].u, a[1].u, v) :
			     glf_get_tex_parameter(st, a[0].u, a[1].u, v);
		if (!n) {
			glf_error(st, GL_INVALID_ENUM);
			return 0;
		}
		bool ints = fn == F_glGetTexParameteriv || fn == F_glGetTexEnviv || fn == F_glGetTexLevelParameteriv;
		put_values(m, level ? a[3].u : a[2].u, ints ? 'i' : 'f', ints && n == 4 ? 0x0B00 : 0, v, n);
		return 0;
	}
	case F_glPixelStorei: glf_pixel_store(st, a[0].u, a[1].i); return 0;
	case F_glPixelStoref: glf_pixel_store(st, a[0].u, (int32_t)floorf(a[1].f + 0.5f)); return 0;
	case F_glReadPixels: {
		size_t bytes = glf_image_bytes(&st->pack, a[4].u, a[5].u, a[2].i, a[3].i);
		if (!bytes) {
			glf_error(st, GL_INVALID_ENUM);
			return 0;
		}
		if (a[2].i < 0 || a[3].i < 0) {
			glf_error(st, GL_INVALID_VALUE);
			return 0;
		}
		if (bytes > (64u << 20) || !writable(m, a[6].u, (uint32_t)bytes))
			return 0;
		if (x->has_surface && !surface_finish(&x->surface))
			lose_gpu(o);
		glf_read_pixels(st, &t, a[0].i, a[1].i, a[2].i, a[3].i, a[4].u, a[5].u, mem_ptr(m, a[6].u));
		return 0;
	}
	case F_glReadBuffer:
	case F_glDrawBuffer:
		return 0;                       /* (one buffer to draw in and read: the back buffer, shown when swapped) */
	case F_glClientActiveTextureARB:
		if (a[0].u - GL_TEXTURE0_ARB < GLF_TEXTURE_UNITS)
			st->client_active_texture = a[0].u - GL_TEXTURE0_ARB;
		else
			glf_error(st, GL_INVALID_ENUM);
		return 0;
	/* lighting, fog, texture coordinate generation, clip planes */
	case F_glLightf: case F_glLighti: case F_glLightModelf: case F_glLightModeli: case F_glMaterialf:
	case F_glMateriali: case F_glFogf: case F_glFogi: case F_glTexGenf: case F_glTexGeni: case F_glTexGend: {
		bool two = fn == F_glLightModelf || fn == F_glLightModeli || fn == F_glFogf || fn == F_glFogi;
		uint32_t pname = a[two ? 0 : 1].u;
		const union arg *val = &a[two ? 1 : 2];
		char type = library_functions[fn].args[two ? 1 : 2];
		float v[4] = { type == 'f' ? val->f : type == 'd' ? (float)val->d : (float)val->i, 0, 0, 0 };
		if (pname_values(pname) != 1) {
			glf_error(st, GL_INVALID_ENUM);         /* (a vector's: the v forms only) */
			return 0;
		}
		if (fn == F_glLightf || fn == F_glLighti)
			glf_light(st, a[0].u, pname, v);
		else if (fn == F_glMaterialf || fn == F_glMateriali)
			glf_material(st, a[0].u, pname, v);
		else if (fn == F_glLightModelf || fn == F_glLightModeli)
			glf_light_model(st, pname, v);
		else if (fn == F_glFogf || fn == F_glFogi)
			glf_fog(st, pname, v);
		else {
			double d = fn == F_glTexGend ? val->d : v[0];
			glf_tex_gen(st, a[0].u, pname, &d);
		}
		return 0;
	}
	case F_glLightfv: case F_glLightiv: case F_glMaterialfv: case F_glMaterialiv: case F_glLightModelfv:
	case F_glLightModeliv: case F_glFogfv: case F_glFogiv: case F_glTexGenfv: case F_glTexGeniv:
	case F_glTexGendv: {
		bool two = fn == F_glLightModelfv || fn == F_glLightModeliv || fn == F_glFogfv || fn == F_glFogiv;
		uint32_t pname = a[two ? 0 : 1].u;
		const char *name = library_functions[fn].name;
		char type = name[strlen(name) - 2];             /* ('f', 'i' or 'd': ...fv, ...iv, ...dv) */
		float v[4] = { 0, 0, 0, 0 };
		if (!param_vector(m, a[two ? 1 : 2].u, type, pname, pname_values(pname), v))
			return 0;
		if (fn == F_glLightfv || fn == F_glLightiv)
			glf_light(st, a[0].u, pname, v);
		else if (fn == F_glMaterialfv || fn == F_glMaterialiv)
			glf_material(st, a[0].u, pname, v);
		else if (two && (fn == F_glLightModelfv || fn == F_glLightModeliv))
			glf_light_model(st, pname, v);
		else if (two)
			glf_fog(st, pname, v);
		else if (fn == F_glTexGendv) {
			double d[4];
			if (!readable(m, a[2].u, 8 * pname_values(pname)))
				return 0;
			for (unsigned k = 0; k < pname_values(pname); k++) {
				uint64_t b = mem_r64(m, a[2].u + 8 * k);
				memcpy(&d[k], &b, 8);
			}
			glf_tex_gen(st, a[0].u, pname, d);
		} else {
			double d[4] = { v[0], v[1], v[2], v[3] };
			glf_tex_gen(st, a[0].u, pname, d);
		}
		return 0;
	}
	case F_glColorMaterial: glf_color_material(st, a[0].u, a[1].u); return 0;
	case F_glClipPlane: {
		double eq[4];
		if (!readable(m, a[1].u, 32))
			return 0;
		for (int k = 0; k < 4; k++) {
			uint64_t b = mem_r64(m, a[1].u + 8 * (uint32_t)k);
			memcpy(&eq[k], &b, 8);
		}
		glf_clip_plane(st, a[0].u, eq);
		return 0;
	}
	case F_glGetLightfv: case F_glGetLightiv: case F_glGetMaterialfv: case F_glGetMaterialiv:
	case F_glGetTexGenfv: case F_glGetTexGeniv: case F_glGetTexGendv: case F_glGetClipPlane: {
		double v[4];
		unsigned n = fn == F_glGetLightfv || fn == F_glGetLightiv ? glf_get_light(st, a[0].u, a[1].u, v) :
			     fn == F_glGetMaterialfv || fn == F_glGetMaterialiv ? glf_get_material(st, a[0].u, a[1].u, v) :
			     fn == F_glGetClipPlane ? glf_get_clip_plane(st, a[0].u, v) :
			     glf_get_tex_gen(st, a[0].u, a[1].u, v);
		char type = fn == F_glGetTexGendv || fn == F_glGetClipPlane ? 'd' :
			    fn == F_glGetLightiv || fn == F_glGetMaterialiv || fn == F_glGetTexGeniv ? 'i' : 'f';
		if (n)
			put_params(m, fn == F_glGetClipPlane ? a[1].u : a[2].u, type, fn == F_glGetClipPlane ? 0 : a[1].u,
				   v, n);
		return 0;
	}
	/* polygons, lines, hints, stencil */
	case F_glPolygonMode: glf_polygon_mode(st, a[0].u, a[1].u); return 0;
	case F_glPolygonOffset: glf_polygon_offset(st, a[0].f, a[1].f); return 0;
	case F_glLineStipple: glf_line_stipple(st, a[0].i, (uint16_t)a[1].u); return 0;
	case F_glPolygonStipple:
		if (readable(m, a[0].u, sizeof st->polygon_stipple))
			memcpy(st->polygon_stipple, mem_ptr(m, a[0].u), sizeof st->polygon_stipple);
		return 0;
	case F_glGetPolygonStipple:
		if (writable(m, a[0].u, sizeof st->polygon_stipple))
			mem_load(m, a[0].u, st->polygon_stipple, sizeof st->polygon_stipple);
		return 0;
	case F_glStencilFunc: glf_stencil_func(st, a[0].u, a[1].i, a[2].u); return 0;
	case F_glStencilOp: glf_stencil_op(st, a[0].u, a[1].u, a[2].u); return 0;
	case F_glStencilMask: glf_stencil_mask(st, a[0].u); return 0;
	case F_glClearStencil: st->clear_stencil = a[0].i; return 0;
	case F_glPushAttrib: glf_push_attrib(st, a[0].u); return 0;
	case F_glPopAttrib: glf_pop_attrib(st); return 0;
	case F_glPushClientAttrib: glf_push_client_attrib(st, a[0].u); return 0;
	case F_glPopClientAttrib: glf_pop_client_attrib(st); return 0;
	/* client arrays */
	case F_glEnableClientState:
	case F_glDisableClientState:
		if (!glf_enable_client(st, a[0].u, fn == F_glEnableClientState))
			glf_error(st, GL_INVALID_ENUM);
		return 0;
	case F_glVertexPointer: glf_array_pointer(st, GLF_ARRAY_VERTEX, a[0].i, a[1].u, a[2].i, a[3].u); return 0;
	case F_glColorPointer: glf_array_pointer(st, GLF_ARRAY_COLOR, a[0].i, a[1].u, a[2].i, a[3].u); return 0;
	case F_glTexCoordPointer: glf_array_pointer(st, GLF_ARRAY_TEX0, a[0].i, a[1].u, a[2].i, a[3].u); return 0;
	case F_glSecondaryColorPointerEXT:
		glf_array_pointer(st, GLF_ARRAY_SECONDARY, a[0].i, a[1].u, a[2].i, a[3].u);
		return 0;
	case F_glNormalPointer: glf_array_pointer(st, GLF_ARRAY_NORMAL, 3, a[0].u, a[1].i, a[2].u); return 0;
	case F_glIndexPointer: glf_array_pointer(st, GLF_ARRAY_INDEX, 1, a[0].u, a[1].i, a[2].u); return 0;
	case F_glEdgeFlagPointer: glf_array_pointer(st, GLF_ARRAY_EDGE, 1, GL_UNSIGNED_BYTE, a[0].i, a[1].u); return 0;
	case F_glInterleavedArrays: glf_interleaved_arrays(st, a[0].u, a[1].i, a[2].u); return 0;
	case F_glLockArraysEXT: glf_lock_arrays(st, a[0].i, a[1].i); return 0;
	case F_glUnlockArraysEXT: glf_unlock_arrays(st); return 0;
	case F_glGetPointerv: {
		uint32_t p;
		if (!glf_get_pointer(st, a[0].u, &p)) {
			if (a[0].u != 0x0DF0 && a[0].u != 0x0DF3) {     /* (feedback and selection buffers: none) */
				glf_error(st, GL_INVALID_ENUM);
				return 0;
			}
			p = 0;
		}
		if (writable(m, a[1].u, 4))
			mem_w32(m, a[1].u, p);
		return 0;
	}
	case F_glArrayElement:
		arrays_at(o, st, a[0].i < 0 ? 0 : a[0].u, 1);
		glf_array_element(st, &t, a[0].i);
		return 0;
	case F_glDrawArrays:
		if (a[1].i >= 0 && a[2].i > 0)
			arrays_at(o, st, a[1].u, a[2].u);
		glf_draw_arrays(st, &t, a[0].u, a[1].i, a[2].i);
		return 0;
	case F_glDrawElements: draw_elements(o, x, a[0].u, a[1].i, a[2].u, a[3].u); return 0;
	case F_glDrawRangeElements:
		if (a[2].u < a[1].u) {
			glf_error(st, GL_INVALID_VALUE);
			return 0;
		}
		draw_elements(o, x, a[0].u, a[3].i, a[4].u, a[5].u);
		return 0;
	default:
		not_yet(o, fn);
		return 0;
	}
}

/* ---- doing records ------------------------------------------------------------------- */

/* A record's arguments (after its first word), by the types recorded */
static unsigned record_args(const uint8_t *p, const char *types, union arg *a)
{
	unsigned k = 0;
	for (; types[k]; k++) {
		switch (types[k]) {
		case 'f': {
			uint32_t w = be32(p);
			memcpy(&a[k].f, &w, 4);
			p += 4;
			break;
		}
		case 'd': {
			uint64_t q = be64(p);
			memcpy(&a[k].d, &q, 8);
			p += 8;
			break;
		}
		case 'b': a[k].i = (int8_t)be32(p); p += 4; break;
		case 'B': a[k].u = be32(p) & 0xFF; p += 4; break;
		case 's': a[k].i = (int16_t)be32(p); p += 4; break;
		case 'S': a[k].u = be32(p) & 0xFFFF; p += 4; break;
		default: a[k].u = be32(p); p += 4; break;
		}
	}
	return k;
}

/* The record a call of fn (after `skip` words: the dispatch table's context) makes; false if it can't be read */
static bool make_record(struct opengl *o, const struct ndrv_call *c, unsigned fn, unsigned skip, uint8_t *r)
{
	const struct recorded *rc = &recorded[fn];
	struct ndrv_args it = ndrv_args(c);
	for (unsigned k = 0; k < skip; k++)
		ndrv_next_word(&it);
	put_be32(r, (uint32_t)fn << 16 | rc->size);
	uint8_t *p = r + 4;
	for (const char *s = library_functions[fn].args; *s; s++) {
		switch (*s) {
		case 'f': put_be32(p, ndrv_next_float(&it)); p += 4; break;
		case 'd': put_be64(p, ndrv_next_double(&it)); p += 8; break;
		case 'p': {
			const char type = rc->args[strlen(rc->args) - 1];
			uint32_t at = ndrv_next_word(&it), size = type == 'd' ? 8 : type == 'b' || type == 'B' ? 1 :
								    type == 's' || type == 'S' ? 2 : 4;
			if (!readable(o->mem, at, rc->count * size))
				return false;
			for (unsigned e = 0; e < rc->count; e++, at += size) {
				if (size == 8) {
					put_be64(p, mem_r64(o->mem, at));
					p += 8;
				} else {
					put_be32(p, size == 1 ? mem_r8(o->mem, at) : size == 2 ? mem_r16(o->mem, at) :
						 mem_r32(o->mem, at));
					p += 4;
				}
			}
			break;
		}
		default: put_be32(p, ndrv_next_word(&it)); p += 4; break;
		}
	}
	return true;
}

/* Does a record of fn's (to x, if there's a context) */
static void run_record(struct opengl *o, struct opengl_context *x, unsigned fn, const uint8_t *r)
{
	union arg a[24];
	unsigned n = record_args(r + 4, recorded[fn].args, a);
	if (o->log)
		log_call("gl", library_functions[fn].name, recorded[fn].args, a, n, "");
	if (x)
		gl_call(o, x, fn, a);
}

/* The context current in an instance, or NULL */
static struct opengl_context *current(struct opengl *o, struct opengl_instance *in)
{
	uint32_t at = data_of(o, in) + D_CURRENT;
	return readable(o->mem, at, 4) ? context_at(o, in, mem_r32(o->mem, at)) : NULL;
}

/* Starts an instance's buffer afresh */
static void record_start(struct opengl *o, struct opengl_instance *in)
{
	uint32_t data = data_of(o, in);
	put_word(o->mem, data + D_RECORD, data + lib.record);
	put_word(o->mem, data + D_RECORD_END, data + lib.record + OPENGL_RECORD_SIZE);
}

/* Does what's recorded in an instance's buffer (to its current context), which is then empty */
static void drain(struct opengl *o, struct opengl_instance *in)
{
	struct mem *m = o->mem;
	uint32_t data = data_of(o, in), start = data + lib.record;
	if (!readable(m, data, D_STRINGS) || !readable(m, start, OPENGL_RECORD_SIZE))
		return;
	uint32_t at = mem_r32(m, data + D_RECORD);
	if (at != start && at - start <= OPENGL_RECORD_SIZE) {
		struct opengl_context *x = current(o, in);
		const uint8_t *p = mem_ptr(m, start), *end = mem_ptr(m, at);
		while (p < end) {
			uint32_t h = (uint32_t)(end - p) >= 4 ? be32(p) : 0;
			unsigned fn = h >> 16, size = h & 0xFFFF;
			if (fn >= GL_FUNCTIONS || !recorded[fn].size || size != recorded[fn].size ||
			    size > (size_t)(end - p)) {
				o->bad_records++;               /* (not ours: the rest dropped) */
				break;
			}
			o->recorded++;
			run_record(o, x, fn, p);
			p += size;
		}
	}
	record_start(o, in);
}

/* ---- AGL ------------------------------------------------------------------------------- */

static void agl_error(struct opengl_instance *in, uint32_t e)
{
	in->agl_error = e;
}

/* A device's depth (bits a pixel), or 0 */
static unsigned device_depth(const struct mem *m, uint32_t gdh)
{
	if (!readable(m, gdh, 4))
		return 0;
	uint32_t gd = mem_r32(m, gdh);
	if (!readable(m, gd, 26))
		return 0;
	uint32_t pmh = mem_r32(m, gd + 22);
	if (!readable(m, pmh, 4))
		return 0;
	uint32_t pm = mem_r32(m, pmh);
	return readable(m, pm, 34) ? mem_r16(m, pm + 32) : 0;
}

#define LM_MAIN_DEVICE 0x08A4u

/* The pixel format at this address (in this instance), or 0 */
static uint32_t format_at(struct opengl *o, struct opengl_instance *in, uint32_t pf)
{
	uint32_t first = data_of(o, in) + lib.formats;
	if (pf < first || (pf - first) % FORMAT_SIZE || (pf - first) / FORMAT_SIZE >= FORMATS ||
	    mem_r32(o->mem, pf + PF_MAGIC) != FORMAT_MAGIC)
		return 0;
	return pf;
}

static uint32_t renderer_info_at(struct opengl *o, struct opengl_instance *in, uint32_t ri)
{
	uint32_t first = data_of(o, in) + lib.renderer_infos;
	if (ri < first || (ri - first) % RENDERER_INFO_SIZE || (ri - first) / RENDERER_INFO_SIZE >= RENDERER_INFOS ||
	    mem_r32(o->mem, ri) != RENDERER_INFO_MAGIC)
		return 0;
	return ri;
}

/*
 * aglChoosePixelFormat(): one renderer, accelerated, RGBA in the device's 16
 * or 32 bits, single or double buffered, depth to 32 bits, stencil to 8, no
 * accumulation, auxiliary or stereo buffers; no 8-bit devices.
 */
static uint32_t choose_pixel_format(struct opengl *o, struct opengl_instance *in, uint32_t devices, int32_t ndev,
				    uint32_t attribs)
{
	struct mem *m = o->mem;
	uint32_t flags = PF_WINDOW, depth = 0, stencil = 0, pixel = 0;
	bool rgba = false;
	for (unsigned k = 0; k < 256; k++) {
		if (!readable(m, attribs + 4 * k, 4)) {
			agl_error(in, AGL_BAD_POINTER);
			return 0;
		}
		uint32_t at = mem_r32(m, attribs + 4 * k), v = 0;
		if (at == AGL_NONE)
			break;
		bool valued = (at >= AGL_BUFFER_SIZE && at <= AGL_ACCUM_ALPHA_SIZE && at != AGL_RGBA &&
			       at != AGL_DOUBLEBUFFER && at != AGL_STEREO) || at == AGL_PIXEL_SIZE ||
			      at == AGL_RENDERER_ID || at == AGL_SAMPLE_BUFFERS_ARB || at == AGL_SAMPLES_ARB;
		if (valued) {
			k++;
			if (!readable(m, attribs + 4 * k, 4)) {
				agl_error(in, AGL_BAD_POINTER);
				return 0;
			}
			v = mem_r32(m, attribs + 4 * k);
		}
		switch (at) {
		case AGL_RGBA: rgba = true; break;
		case AGL_DOUBLEBUFFER: flags |= PF_DOUBLE; break;
		case AGL_DEPTH_SIZE: depth = v; break;
		case AGL_STENCIL_SIZE: stencil = v; break;
		case AGL_PIXEL_SIZE: case AGL_BUFFER_SIZE: pixel = v; break;
		case AGL_FULLSCREEN: flags |= PF_FULLSCREEN; break;
		case AGL_OFFSCREEN: flags |= PF_OFFSCREEN; break;
		case AGL_WINDOW: flags |= PF_WINDOW; break;
		case AGL_NO_RECOVERY: flags |= PF_NO_RECOVERY; break;
		case AGL_ALL_RENDERERS: flags |= PF_ALL_RENDERERS; break;
		case AGL_MINIMUM_POLICY: flags |= 0x40; break;
		case AGL_MAXIMUM_POLICY: flags |= 0x80; break;
		case AGL_CLOSEST_POLICY: flags |= 0x100; break;
		case AGL_STEREO:
			return 0;
		case AGL_LEVEL: case AGL_AUX_BUFFERS: case AGL_ACCUM_RED_SIZE: case AGL_ACCUM_GREEN_SIZE:
		case AGL_ACCUM_BLUE_SIZE: case AGL_ACCUM_ALPHA_SIZE: case AGL_SAMPLE_BUFFERS_ARB: case AGL_SAMPLES_ARB:
			if (v)
				return 0;       /* (none of those) */
			break;
		case AGL_RENDERER_ID:
			if (v != RENDERER_ID)
				return 0;
			break;
		case AGL_RED_SIZE: case AGL_GREEN_SIZE: case AGL_BLUE_SIZE: case AGL_ALPHA_SIZE:
			if (v > 8)
				return 0;
			break;
		case AGL_ACCELERATED: case AGL_SINGLE_RENDERER: case AGL_ROBUST: case AGL_BACKING_STORE: case AGL_MP_SAFE:
		case AGL_MULTISCREEN: case AGL_VIRTUAL_SCREEN: case AGL_COMPLIANT:
			break;
		default:
			agl_error(in, AGL_BAD_ATTRIBUTE);
			return 0;
		}
	}
	if (!rgba || depth > 32 || stencil > 8)
		return 0;
	/* the devices (none given: the main device) must all be 16 or 32 bits a pixel */
	uint32_t device = 0;
	if (devices && ndev > 0) {
		for (int32_t k = 0; k < ndev && k < 16; k++) {
			if (!readable(m, devices + 4 * (uint32_t)k, 4)) {
				agl_error(in, AGL_BAD_GDEV);
				return 0;
			}
			uint32_t gdh = mem_r32(m, devices + 4 * (uint32_t)k);
			unsigned bits = device_depth(m, gdh);
			if (!bits) {
				agl_error(in, AGL_BAD_GDEV);
				return 0;
			}
			if (bits != 16 && bits != 32)
				return 0;
			device = device ? device : gdh;
		}
	} else {
		device = readable(m, LM_MAIN_DEVICE, 4) ? mem_r32(m, LM_MAIN_DEVICE) : 0;
		unsigned bits = device_depth(m, device);
		if (bits && bits != 16 && bits != 32 && !(flags & (PF_FULLSCREEN | PF_OFFSCREEN)))
			return 0;
	}
	unsigned bits = device_depth(m, device);
	pixel = bits == 16 || bits == 32 ? bits : pixel > 16 || !pixel ? 32 : 16;
	depth = !depth ? 0 : depth <= 16 ? 16 : depth <= 24 ? 24 : 32;
	stencil = stencil ? 8 : 0;
	uint32_t first = data_of(o, in) + lib.formats;
	for (unsigned k = 0; k < FORMATS; k++) {
		uint32_t pf = first + k * FORMAT_SIZE;
		if (mem_r32(m, pf + PF_MAGIC) == FORMAT_MAGIC)
			continue;
		if (!writable(m, pf, FORMAT_SIZE))
			break;
		memset(mem_ptr(m, pf), 0, FORMAT_SIZE);
		mem_w32(m, pf + PF_MAGIC, FORMAT_MAGIC);
		mem_w32(m, pf + PF_FLAGS, flags);
		mem_w32(m, pf + PF_DEPTH, depth);
		mem_w32(m, pf + PF_STENCIL, stencil);
		mem_w32(m, pf + PF_PIXEL, pixel);
		mem_w32(m, pf + PF_DEVICE, device);
		return pf;
	}
	agl_error(in, AGL_BAD_ALLOC);
	return 0;
}

/* aglDescribePixelFormat(): false for an attribute it doesn't know */
static bool describe_pixel_format(const struct mem *m, uint32_t pf, uint32_t attrib, int32_t *v)
{
	uint32_t flags = mem_r32(m, pf + PF_FLAGS), pixel = mem_r32(m, pf + PF_PIXEL);
	switch (attrib) {
	case AGL_ALL_RENDERERS: *v = (flags & PF_ALL_RENDERERS) != 0; break;
	case AGL_BUFFER_SIZE: case AGL_PIXEL_SIZE: *v = (int32_t)pixel; break;
	case AGL_LEVEL: case AGL_STEREO: case AGL_AUX_BUFFERS: case AGL_ACCUM_RED_SIZE: case AGL_ACCUM_GREEN_SIZE:
	case AGL_ACCUM_BLUE_SIZE: case AGL_ACCUM_ALPHA_SIZE: case AGL_SAMPLE_BUFFERS_ARB: case AGL_SAMPLES_ARB:
	case AGL_ROBUST: case AGL_BACKING_STORE: case AGL_MP_SAFE: case AGL_MULTISCREEN: case AGL_VIRTUAL_SCREEN:
		*v = 0;
		break;
	case AGL_RGBA: case AGL_ACCELERATED: case AGL_SINGLE_RENDERER: case AGL_COMPLIANT: *v = 1; break;
	case AGL_DOUBLEBUFFER: *v = (flags & PF_DOUBLE) != 0; break;
	case AGL_RED_SIZE: case AGL_GREEN_SIZE: case AGL_BLUE_SIZE: *v = pixel == 32 ? 8 : 5; break;
	case AGL_ALPHA_SIZE: *v = pixel == 32 ? 8 : 0; break;
	case AGL_DEPTH_SIZE: *v = (int32_t)mem_r32(m, pf + PF_DEPTH); break;
	case AGL_STENCIL_SIZE: *v = (int32_t)mem_r32(m, pf + PF_STENCIL); break;
	case AGL_MINIMUM_POLICY: *v = (flags & 0x40) != 0; break;
	case AGL_MAXIMUM_POLICY: *v = (flags & 0x80) != 0; break;
	case AGL_CLOSEST_POLICY: *v = (flags & 0x100) != 0; break;
	case AGL_OFFSCREEN: *v = (flags & PF_OFFSCREEN) != 0; break;
	case AGL_FULLSCREEN: *v = (flags & PF_FULLSCREEN) != 0; break;
	case AGL_WINDOW: *v = (flags & PF_WINDOW) != 0; break;
	case AGL_NO_RECOVERY: *v = (flags & PF_NO_RECOVERY) != 0; break;
	case AGL_RENDERER_ID: *v = (int32_t)RENDERER_ID; break;
	default: return false;
	}
	return true;
}

/* aglDescribeRenderer(): the one renderer there is */
static bool describe_renderer(uint32_t prop, int32_t *v)
{
	switch (prop) {
	case AGL_RENDERER_ID: *v = (int32_t)RENDERER_ID; break;
	case AGL_OFFSCREEN: case AGL_FULLSCREEN: case AGL_WINDOW: case AGL_ACCELERATED: case AGL_COMPLIANT: *v = 1; break;
	case AGL_ROBUST: case AGL_BACKING_STORE: case AGL_MP_SAFE: case AGL_MULTISCREEN: case AGL_MIN_LEVEL:
	case AGL_MAX_LEVEL: case AGL_ACCUM_MODES: case AGL_MAX_AUX_BUFFERS:
		*v = 0;
		break;
	case AGL_BUFFER_MODES: *v = AGL_MONOSCOPIC_BIT | AGL_SINGLEBUFFER_BIT | AGL_DOUBLEBUFFER_BIT; break;
	case AGL_COLOR_MODES: *v = AGL_RGB555_BIT | AGL_RGB888_BIT | AGL_ARGB8888_BIT; break;
	case AGL_DEPTH_MODES: *v = AGL_0_BIT | AGL_16_BIT | AGL_24_BIT | AGL_32_BIT; break;
	case AGL_STENCIL_MODES: *v = AGL_0_BIT | AGL_8_BIT; break;
	case AGL_VIDEO_MEMORY: *v = VIDEO_MEMORY; break;
	case AGL_TEXTURE_MEMORY: *v = TEXTURE_MEMORY; break;
	case AGL_RENDERER_COUNT: *v = 1; break;
	default: return false;
	}
	return true;
}

static struct opengl_context *new_context(struct opengl *o, struct opengl_instance *in, uint32_t pf,
					  struct opengl_context *share)
{
	struct mem *m = o->mem;
	unsigned k = 0;
	while (k < OPENGL_CONTEXTS && in->ctx[k].used)
		k++;
	uint32_t guest = data_of(o, in) + lib.contexts + k * CONTEXT_SIZE;
	if (k == OPENGL_CONTEXTS || !writable(m, guest, CONTEXT_SIZE))
		return NULL;
	struct opengl_context *x = &in->ctx[k];
	memset(x, 0, sizeof *x);
	x->used = true;
	x->inst = in;
	x->guest = guest;
	x->format = mem_r32(m, pf + PF_FLAGS);
	x->depth_bits = mem_r32(m, pf + PF_DEPTH);
	x->stencil_bits = mem_r32(m, pf + PF_STENCIL);
	x->pixel_size = mem_r32(m, pf + PF_PIXEL);
	x->swap_interval = 0;
	glf_init(&x->st);
	if (!glf_attach(&x->st, share ? &share->st : NULL)) {
		memset(x, 0, sizeof *x);
		return NULL;
	}
	/* the handle (itself), then the dispatch table: each gl function's TVector, taking the handle first */
	mem_w32(m, guest, guest);
	for (unsigned slot = 0; slot < TABLE_SLOTS; slot++)
		mem_w32(m, guest + 4 + 4 * slot, in->toc + o->library.entry[E_NOTHING]);
	for (unsigned f = 0; f < GL_FUNCTIONS; f++)
		mem_w32(m, guest + library_functions[f].offset, in->toc + o->library.entry[E_TABLE + f]);
	o->contexts++;
	return x;
}

static uint32_t agl_call(struct opengl *o, struct ndrv_call *c, struct opengl_instance *in, unsigned fn,
			 const union arg *a)
{
	struct mem *m = o->mem;
	uint32_t data = data_of(o, in);
	struct opengl_context *x = NULL;
	switch (fn) {
	case F_aglChoosePixelFormat:
		return choose_pixel_format(o, in, a[0].u, a[1].i, a[2].u);
	case F_aglDestroyPixelFormat:
		if (format_at(o, in, a[0].u))
			put_word(m, a[0].u + PF_MAGIC, 0);
		else
			agl_error(in, AGL_BAD_PIXELFMT);
		return 0;
	case F_aglNextPixelFormat:
		if (!format_at(o, in, a[0].u))
			agl_error(in, AGL_BAD_PIXELFMT);
		return 0;
	case F_aglDescribePixelFormat: {
		int32_t v;
		if (!format_at(o, in, a[0].u)) {
			agl_error(in, AGL_BAD_PIXELFMT);
			return 0;
		}
		if (!describe_pixel_format(m, a[0].u, a[1].u, &v)) {
			agl_error(in, AGL_BAD_ATTRIBUTE);
			return 0;
		}
		put_word(m, a[2].u, (uint32_t)v);
		return 1;
	}
	case F_aglDevicesOfPixelFormat:
		if (!format_at(o, in, a[0].u)) {
			agl_error(in, AGL_BAD_PIXELFMT);
			return 0;
		}
		put_word(m, a[1].u, mem_r32(m, a[0].u + PF_DEVICE) ? 1 : 0);
		return a[0].u + PF_DEVICE;
	case F_aglQueryRendererInfo: {
		uint32_t first = data + lib.renderer_infos;
		for (unsigned k = 0; k < RENDERER_INFOS; k++) {
			uint32_t ri = first + k * RENDERER_INFO_SIZE;
			if (mem_r32(m, ri) != RENDERER_INFO_MAGIC && writable(m, ri, RENDERER_INFO_SIZE)) {
				mem_w32(m, ri, RENDERER_INFO_MAGIC);
				return ri;
			}
		}
		agl_error(in, AGL_BAD_ALLOC);
		return 0;
	}
	case F_aglDestroyRendererInfo:
		if (renderer_info_at(o, in, a[0].u))
			put_word(m, a[0].u, 0);
		else
			agl_error(in, AGL_BAD_RENDINFO);
		return 0;
	case F_aglNextRendererInfo:
		if (!renderer_info_at(o, in, a[0].u))
			agl_error(in, AGL_BAD_RENDINFO);
		return 0;
	case F_aglDescribeRenderer: {
		int32_t v;
		if (!renderer_info_at(o, in, a[0].u)) {
			agl_error(in, AGL_BAD_RENDINFO);
			return 0;
		}
		if (!describe_renderer(a[1].u, &v)) {
			agl_error(in, AGL_BAD_PROPERTY);
			return 0;
		}
		put_word(m, a[2].u, (uint32_t)v);
		return 1;
	}
	case F_aglCreateContext:
		if (!format_at(o, in, a[0].u)) {
			agl_error(in, AGL_BAD_PIXELFMT);
			return 0;
		}
		if (a[1].u && !context_at(o, in, a[1].u)) {
			agl_error(in, AGL_BAD_MATCH);
			return 0;
		}
		x = new_context(o, in, a[0].u, context_at(o, in, a[1].u));
		if (!x) {
			agl_error(in, AGL_BAD_ALLOC);
			return 0;
		}
		return x->guest;
	case F_aglGetVersion:           /* (1.1, as Mac OS 9.2.2's) */
		put_word(m, a[0].u, 1);
		put_word(m, a[1].u, 1);
		return 0;
	case F_aglConfigure:
		return 1;
	case F_aglGetError: {
		uint32_t e = in->agl_error;
		in->agl_error = AGL_NO_ERROR;
		return e;
	}
	case F_aglErrorString:
		return data + (a[0].u == AGL_NO_ERROR ? lib.no_error :
			       a[0].u >= AGL_BAD_ATTRIBUTE && a[0].u < AGL_ERRORS ? lib.agl_error[a[0].u - AGL_BAD_ATTRIBUTE] :
			       lib.unknown_error);
	case F_aglResetLibrary: {
		uint32_t toc = in->toc;
		free_instance(o, in);
		in->toc = toc;
		put_word(m, data + D_CURRENT, 0);
		return 0;
	}
	case F_aglGetCurrentContext:
		return readable(m, data + D_CURRENT, 4) ? mem_r32(m, data + D_CURRENT) : 0;
	case F_aglSetCurrentContext:
		if (a[0].u && !context_at(o, in, a[0].u)) {
			agl_error(in, AGL_BAD_CONTEXT);
			return 0;
		}
		put_word(m, data + D_CURRENT, a[0].u);
		return 1;
	case F_aglUseFont:
	case F_aglSetFullScreen:
		if (!o->unimplemented[fn]) {
			o->unimplemented[fn] = 1;
			printf("opengl: %s isn't done yet\n", library_functions[fn].name);
			fflush(stdout);
		}
		break;                  /* (checks the context, then fails) */
	default:
		break;
	}

	/* the rest take a context first */
	x = context_at(o, in, a[0].u);
	if (!x) {
		agl_error(in, AGL_BAD_CONTEXT);
		return 0;
	}
	switch (fn) {
	case F_aglDestroyContext:
		if (readable(m, data + D_CURRENT, 4) && mem_r32(m, data + D_CURRENT) == x->guest)
			put_word(m, data + D_CURRENT, 0);
		put_word(m, x->guest, 0);
		free_context(o, x);
		return 1;
	case F_aglCopyContext:
		if (!context_at(o, in, a[1].u)) {
			agl_error(in, AGL_BAD_CONTEXT);
			return 0;
		}
		glf_copy(&context_at(o, in, a[1].u)->st, &x->st);      /* (all of it, whatever the mask) */
		return 1;
	case F_aglUpdateContext:
		if (x->kind == DRAWABLE_PORT)
			fit_drawable(o, x);
		return 1;
	case F_aglSetDrawable:
		if (!a[1].u) {
			x->kind = DRAWABLE_NONE;
			x->port = 0;
			return 1;
		}
		{
			int r[4];
			if (!port_rect(m, a[1].u, r)) {
				agl_error(in, AGL_BAD_DRAWABLE);
				return 0;
			}
		}
		x->kind = DRAWABLE_PORT;
		x->port = a[1].u;
		if (!fit_drawable(o, x)) {
			x->kind = DRAWABLE_NONE;
			x->port = 0;
			agl_error(in, AGL_BAD_ALLOC);
			return 0;
		}
		return 1;
	case F_aglSetOffScreen:
		if (a[1].i <= 0 || a[2].i <= 0 || (uint32_t)a[3].i < (uint32_t)a[1].i * x->pixel_size / 8 || !a[4].u) {
			agl_error(in, AGL_BAD_OFFSCREEN);
			return 0;
		}
		x->kind = DRAWABLE_OFFSCREEN;
		x->port = 0;
		x->base = a[4].u;
		x->row_bytes = (uint32_t)a[3].i;
		x->width = a[1].i;              /* (fit_drawable() takes these) */
		x->height = a[2].i;
		x->origin_x = x->origin_y = 0;
		if (x->has_surface && (x->surface.target.width != a[1].i || x->surface.target.height != a[2].i)) {
			surface_free(&x->surface);
			x->has_surface = false;
		}
		if (!fit_drawable(o, x)) {
			x->kind = DRAWABLE_NONE;
			agl_error(in, AGL_BAD_ALLOC);
			return 0;
		}
		return 1;
	case F_aglSetFullScreen:
		agl_error(in, AGL_BAD_FULLSCREEN);
		return 0;
	case F_aglGetDrawable:
		return x->kind == DRAWABLE_PORT ? x->port : 0;
	case F_aglSetVirtualScreen:
		if (a[1].i) {
			agl_error(in, AGL_BAD_VALUE);
			return 0;
		}
		return 1;
	case F_aglGetVirtualScreen:
		return 0;
	case F_aglSwapBuffers:
		show(o, x);
		if (x->kind == DRAWABLE_PORT) {         /* (for the screen: paced by its refresh, see nk.c) */
			o->shown = 1 + (unsigned)(in - o->inst) * OPENGL_CONTEXTS + (unsigned)(x - in->ctx);
			o->shown_id = x->guest;
		}
		return 0;
	case F_aglEnable:
	case F_aglDisable:
	case F_aglIsEnabled: {
		static const uint32_t names[] = { AGL_SWAP_RECT, AGL_BUFFER_RECT, AGL_SWAP_LIMIT, AGL_COLORMAP_TRACKING,
						  AGL_RASTERIZATION, AGL_STATE_VALIDATION };
		unsigned k = 0;
		while (k < sizeof names / sizeof names[0] && names[k] != a[1].u)
			k++;
		if (k == sizeof names / sizeof names[0]) {
			agl_error(in, AGL_BAD_ENUM);
			return 0;
		}
		if (fn == F_aglIsEnabled)
			return (x->enabled >> k & 1) != 0;
		bool was = x->enabled >> k & 1;
		x->enabled = fn == F_aglEnable ? x->enabled | 1u << k : x->enabled & ~(1u << k);
		if (k == OPTION_BUFFER_RECT && was != (fn == F_aglEnable) && x->kind == DRAWABLE_PORT)
			fit_drawable(o, x);
		return 1;
	}
	case F_aglSetInteger:
	case F_aglGetInteger: {
		int32_t *v = a[1].u == AGL_SWAP_RECT ? x->swap_rect : a[1].u == AGL_BUFFER_RECT ? x->buffer_rect :
			     a[1].u == AGL_SWAP_INTERVAL ? &x->swap_interval : NULL;
		unsigned n = a[1].u == AGL_SWAP_RECT || a[1].u == AGL_BUFFER_RECT ? 4 : 1;
		if (!v) {
			if (a[1].u == AGL_SWAP_LIMIT || a[1].u == AGL_COLORMAP_ENTRY || a[1].u == AGL_RASTERIZATION ||
			    a[1].u == AGL_STATE_VALIDATION) {
				if (fn == F_aglGetInteger)
					put_word(m, a[2].u, 0);
				return 1;
			}
			agl_error(in, AGL_BAD_ENUM);
			return 0;
		}
		if (!(fn == F_aglSetInteger ? readable(m, a[2].u, 4 * n) : writable(m, a[2].u, 4 * n))) {
			agl_error(in, AGL_BAD_POINTER);
			return 0;
		}
		for (unsigned k = 0; k < n; k++) {
			if (fn == F_aglSetInteger)
				v[k] = (int32_t)mem_r32(m, a[2].u + 4 * k);
			else
				mem_w32(m, a[2].u + 4 * k, (uint32_t)v[k]);
		}
		if (fn == F_aglSetInteger && a[1].u == AGL_BUFFER_RECT && x->enabled & 1u << OPTION_BUFFER_RECT &&
		    x->kind == DRAWABLE_PORT)
			fit_drawable(o, x);
		return 1;
	}
	case F_aglUseFont:
	default:
		(void)c;
		return 0;
	}
}

/* ---- calls ----------------------------------------------------------------------------- */

static void finish_call(struct ndrv_call *c, const char *result, uint32_t v)
{
	ndrv_finish(c, result[0] == 'v' ? 0 : result[0] == 'B' ? (v ? 1 : 0) : v);
}

void opengl_library_call(struct opengl *o, struct ndrv_call *c)
{
	o->calls++;
	o->shown = 0;
	if (c->entry == E_INIT || c->entry == E_TERM) {
		/* CFM's initialization (a new process: its instance from scratch) and termination */
		struct opengl_instance *in = instance_of(o, c->toc, c->entry == E_INIT);
		if (in) {
			uint32_t toc = c->toc;
			free_instance(o, in);
			if (c->entry == E_INIT) {
				in->toc = toc;
				put_word(o->mem, data_of(o, in) + D_CURRENT, 0);
				record_start(o, in);
			}
		}
		ndrv_finish(c, 0);
		return;
	}
	unsigned fn = c->entry, skip = 0;
	if (fn >= E_TABLE && fn < E_LIBRARY) {          /* (through the dispatch table: the handle first) */
		fn -= E_TABLE;
		skip = 1;
	} else if (fn >= F_LIBRARY) {
		ndrv_finish(c, 0);
		return;
	}
	struct opengl_instance *in = instance_of(o, c->toc, true);
	if (!in) {
		ndrv_finish(c, 0);
		return;
	}
	drain(o, in);                                   /* (what was recorded comes first) */
	const struct function *f = &library_functions[fn];
	struct opengl_context *x = fn >= GL_FUNCTIONS ? NULL : skip ? context_at(o, in, ndrv_arg(c, 0)) :
				   current(o, in);
	if (fn < GL_FUNCTIONS && recorded[fn].size) {
		/* (one the library records, as a host call: its record made here, then done as recorded ones are) */
		uint8_t r[RECORD_MAX];
		if (make_record(o, c, fn, skip, r))
			run_record(o, x, fn, r);
		ndrv_finish(c, 0);
		return;
	}
	union arg a[16];
	unsigned n = get_args(c, f->args, skip, a);
	uint32_t result = fn >= GL_FUNCTIONS ? agl_call(o, c, in, fn, a) : x ? gl_call(o, x, fn, a) : 0;
	if (o->log) {
		char what[32] = "";
		if (f->result[0] != 'v')
			snprintf(what, sizeof what, " = 0x%X", result);
		log_call("gl", f->name, f->args, a, n, what);
	}
	finish_call(c, f->result, result);
}

/* OpenGLMemory: blocks from the Memory Manager (state word 0: the step; 1, 2: the call's) */
enum { MS_NONE, MS_REALLOC_SIZE, MS_REALLOC_NEW, MS_REALLOC_DISPOSE, MS_RESULT };

void opengl_memory_call(struct opengl *o, struct ndrv_call *c)
{
	struct mem *m = o->mem;
	if (c->entry == NDRV_ENTRY_RESUME) {
		uint32_t r = c->arg[0], step = ndrv_state(c, 0);
		switch (step) {
		case MS_REALLOC_SIZE:           /* (the old block's size: then a new block) */
			ndrv_set_state(c, 3, r);
			ndrv_set_state(c, 0, MS_REALLOC_NEW);
			ndrv_callout(c, ndrv_import(c, &o->memory, MI_NEW_PTR), ndrv_state(c, 2), 0, 0, 0);
			return;
		case MS_REALLOC_NEW: {          /* (its contents copied, the old one freed) */
			uint32_t old = ndrv_state(c, 1), n = ndrv_state(c, 3), size = ndrv_state(c, 2);
			n = n < size ? n : size;
			if (!r) {
				ndrv_finish(c, 0);
				return;
			}
			if (readable(m, old, n) && writable(m, r, n))
				memmove(mem_ptr(m, r), mem_ptr(m, old), n);
			ndrv_set_state(c, 1, r);
			ndrv_set_state(c, 0, MS_RESULT);
			ndrv_callout(c, ndrv_import(c, &o->memory, MI_DISPOSE_PTR), old, 0, 0, 0);
			return;
		}
		case MS_RESULT:
			ndrv_finish(c, ndrv_state(c, 1));
			return;
		default:
			ndrv_finish(c, r);
			return;
		}
	}
	if (c->entry >= M_LIBRARY) {
		ndrv_finish(c, 0);
		return;
	}
	const struct function *f = &memory_functions[c->entry];
	union arg a[8];
	unsigned n = get_args(c, f->args, 0, a);
	if (o->log)
		log_call("glm", f->name, f->args, a, n, "");
	ndrv_set_state(c, 0, MS_NONE);
	switch (c->entry) {
	case M_glmMalloc: case M_glmVecAlloc: case M_glmDCBAlloc:
		ndrv_callout(c, ndrv_import(c, &o->memory, MI_NEW_PTR), a[0].u, 0, 0, 0);
		return;
	case M_glmCalloc:
		ndrv_callout(c, ndrv_import(c, &o->memory, MI_NEW_PTR_CLEAR), a[0].u * a[1].u, 0, 0, 0);
		return;
	case M_glmFree: case M_glmVecFree: case M_glmDCBFree:
		if (a[0].u)
			ndrv_callout(c, ndrv_import(c, &o->memory, MI_DISPOSE_PTR), a[0].u, 0, 0, 0);
		else
			ndrv_finish(c, 0);
		return;
	case M_glmRealloc: case M_glmVecRealloc: case M_glmDCBRealloc:
		if (!a[0].u) {
			ndrv_callout(c, ndrv_import(c, &o->memory, MI_NEW_PTR), a[1].u, 0, 0, 0);
			return;
		}
		ndrv_set_state(c, 0, MS_REALLOC_SIZE);
		ndrv_set_state(c, 1, a[0].u);
		ndrv_set_state(c, 2, a[1].u);
		ndrv_callout(c, ndrv_import(c, &o->memory, MI_GET_PTR_SIZE), a[0].u, 0, 0, 0);
		return;
	case M_glmZero:
		if (writable(m, a[0].u, a[1].u))
			memset(mem_ptr(m, a[0].u), 0, a[1].u);
		break;
	case M_glmCopy:
		if (readable(m, a[0].u, a[2].u) && writable(m, a[1].u, a[2].u))
			memmove(mem_ptr(m, a[1].u), mem_ptr(m, a[0].u), a[2].u);
		break;
	case M_glmSetUByte: case M_glmSetUShort: case M_glmSetUInt: case M_glmSetDouble: {
		unsigned size = c->entry == M_glmSetUByte ? 1 : c->entry == M_glmSetUShort ? 2 :
				c->entry == M_glmSetUInt ? 4 : 8;
		if (a[1].u > 0x1000000u || !writable(m, a[0].u, a[1].u * size))
			break;
		for (uint32_t k = 0; k < a[1].u; k++) {
			uint32_t at = a[0].u + k * size;
			if (size == 1)
				mem_w8(m, at, (uint8_t)a[2].u);
			else if (size == 2)
				mem_w16(m, at, (uint16_t)a[2].u);
			else if (size == 4)
				mem_w32(m, at, a[2].u);
			else {
				uint64_t b;
				memcpy(&b, &a[2].d, 8);
				mem_w64(m, at, b);
			}
		}
		break;
	}
	default:                        /* (modes, debugging, status: accepted) */
		break;
	}
	ndrv_finish(c, 0);
}

void opengl_utility_call(struct opengl *o, struct ndrv_call *c)
{
	if (c->entry >= U_LIBRARY) {
		ndrv_finish(c, 0);
		return;
	}
	const struct function *f = &utility_functions[c->entry];
	union arg a[16];
	unsigned n = get_args(c, f->args, 0, a);
	if (o->log)
		log_call("glu", f->name, f->args, a, n, "");
	if (!(o->glu_told >> c->entry & 1)) {
		o->glu_told |= 1ull << c->entry;
		printf("opengl: %s isn't done yet\n", f->name);
		fflush(stdout);
	}
	ndrv_finish(c, 0);
}
