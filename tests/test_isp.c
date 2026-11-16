/*
 * Blue Game Controllers, the InputSprocket driver (isp.h): its fragment and
 * file among the tools; which of a game's needs a pad meets (Unreal
 * Tournament's, and a flight game's); the data it pushes; and the driver as
 * InputSprocket drives it, against a Mac OS of the test's own: a pad
 * registered as it's loaded, a game's needs met, its presses pushed, its
 * configuration kept, its panel shown, and a copy of another version's not
 * run.
 */
#include <string.h>

#include "../src/be.h"
#include "../src/hfs.h"
#include "../src/isp.h"
#include "../src/ppc.h"
#include "../src/tools.h"
#include "test.h"

#define FOURCC ISP_FOURCC
#define HZ 16600000u

/* ---- the fragment ------------------------------------------------------------------- */

static uint8_t frag[ISP_LIBRARY_MAX];
static struct ndrv_layout layout;
static const char *import_name[NDRV_MAX_IMPORTS];
static unsigned imports;
static char strings[8192];

/* Its imports' names, in the order of their TVector pointers, from its loader section */
static void read_imports(size_t len)
{
	const uint8_t *ld = frag + be32(frag + 40 + 56 + 20);
	unsigned libs = be32(ld + 24);
	imports = be32(ld + 28);
	const uint8_t *syms = ld + 56 + 24 * libs, *names = ld + be32(ld + 40);
	size_t at = 0;
	for (unsigned k = 0; k < imports && k < NDRV_MAX_IMPORTS; k++) {
		const char *name = (const char *)names + (be32(syms + 4 * k) & 0xFFFFFF);
		size_t n = strlen(name);
		if (at + n + 1 > sizeof strings || (const uint8_t *)name >= frag + len)
			break;
		import_name[k] = memcpy(strings + at, name, n + 1);
		at += n + 1;
	}
}

static int import_named(const char *name)
{
	for (unsigned k = 0; k < imports; k++)
		if (import_name[k] && !strcmp(import_name[k], name))
			return (int)k;
	return -1;
}

static void test_fragment(void)
{
	size_t len = isp_build(frag, sizeof frag, &layout);
	CHECK(len > 0);
	CHECK_EQ(isp_build(frag, 1024, &layout), 0);
	len = isp_build(frag, sizeof frag, &layout);
	read_imports(len);
	static const char *const wanted[] = { "ISpDevice_New", "ISpElement_New", "ISpElement_PushSimpleData",
					      "InsTime", "PrimeTime", "RmvTime", "AppendDITL", "PopUpMenuSelect" };
	for (size_t k = 0; k < sizeof wanted / sizeof wanted[0]; k++)
		CHECK(import_named(wanted[k]) >= 0);

	/* InputSprocket 1.7's exports, by the hash table */
	const uint8_t *ld = frag + be32(frag + 40 + 56 + 20);
	unsigned power = be32(ld + 48), nexports = be32(ld + 52);
	CHECK_EQ(nexports, 4);
	const uint8_t *names = ld + be32(ld + 40), *hash = ld + be32(ld + 44);
	const uint8_t *keys = hash + 4 * (1u << power), *syms = keys + 4 * nexports;
	static const char *const exported[] = { "ISpDriver_CheckConfiguration", "ISpDriver_FindAndLoadDevices",
						"ISpDriver_DisposeDevices", "ISpDriver_Tickle" };
	static const unsigned entry[] = { ISP_ENTRY_CHECK, ISP_ENTRY_FIND, ISP_ENTRY_DISPOSE, ISP_ENTRY_TICKLE };
	for (unsigned k = 0; k < 4; k++) {
		uint32_t key = pef_hash_word(exported[k], strlen(exported[k])), found = 0;
		uint32_t chain = be32(hash + 4 * ((key ^ key >> power) & ((1u << power) - 1)));
		for (uint32_t j = chain & 0x3FFFF; j < (chain & 0x3FFFF) + (chain >> 18); j++) {
			const uint8_t *sym = syms + 10 * j;
			if (be32(keys + 4 * j) == key && !memcmp(names + (be32(sym) & 0xFFFFFF), exported[k],
								  strlen(exported[k]))) {
				found++;
				CHECK_EQ(be32(sym + 4), layout.entry[entry[k]]);
			}
		}
		CHECK_EQ(found, 1);
	}

	/* the file: a shared library InputSprocket loads ('insp'), with its own icon */
	struct hfs_file files[TOOLS_FILES];
	CHECK_EQ(tools_files(files), 0);
	const struct hfs_file *f = &files[TOOLS_ISP];
	CHECK(!strcmp(f->name, "Blue Game Controllers"));
	CHECK(f->type == FOURCC('s', 'h', 'l', 'b') && f->creator == FOURCC('i', 'n', 's', 'p'));
	CHECK(f->data_len == len && !memcmp(f->data, frag, len));
	bool hostcall = false;
	for (size_t k = 0; k + 4 <= len; k += 4)
		hostcall |= be32(frag + k) == PPC_HOSTCALL_INSN(HOSTCALL_TOOLS_ISP);
	CHECK(hostcall);
}

/* ---- autoconfiguration -------------------------------------------------------------- */

struct need_def {
	const char *name;
	uint32_t kind, label, flags;
};

#define BUTN ISP_KIND_BUTTON
#define AXIS ISP_KIND_AXIS
#define DLTA ISP_KIND_DELTA

/* Unreal Tournament's needs (as its ISpInit gives them) */
static const struct need_def ut[] = {
	{ "Turn", AXIS, FOURCC('x', 'a', 'x', 'i'), 0x50000000 },
	{ "Look/Move", AXIS, FOURCC('y', 'a', 'x', 'i'), 0x50000000 },
	{ "Turn (Mouse Left/Right)", DLTA, FOURCC('x', 'd', 'l', 't'), 0x30000000 },
	{ "Look/Move (Mouse Up/Down)", DLTA, FOURCC('y', 'd', 'l', 't'), 0x30000000 },
	{ "Fire/Click", BUTN, FOURCC('f', 'i', 'r', 'e'), 0 },
	{ "Alt Fire/Alt Click", BUTN, FOURCC('s', 'f', 'i', 'r'), 0 },
	{ "Jump", BUTN, FOURCC('j', 'u', 'm', 'p'), 0 },
	{ "Crouch", BUTN, FOURCC('n', 'o', 'n', 'e'), 0 },
	{ "Previous Weapon", BUTN, FOURCC('b', 't', 'p', 'v'), 0 },
	{ "Next Weapon", BUTN, FOURCC('b', 't', 'n', 'x'), 0 },
	{ "Throw Weapon", BUTN, FOURCC('n', 'o', 'n', 'e'), 0 },
	{ "Mouse Look", BUTN, FOURCC('n', 'o', 'n', 'e'), 0 },
	{ "Previous Inventory", BUTN, FOURCC('b', 't', 'p', 'v'), 0 },
	{ "Next Inventory", BUTN, FOURCC('b', 't', 'n', 'x'), 0 },
	{ "Activate Inventory", BUTN, FOURCC('n', 'o', 'n', 'e'), 0 },
	{ "Walk", BUTN, FOURCC('n', 'o', 'n', 'e'), 0 },
	{ "Step Left", BUTN, FOURCC('b', 'l', 'f', 't'), 0 },
	{ "Step Right", BUTN, FOURCC('b', 'r', 'g', 't'), 0 },
	{ "Sidestep", BUTN, FOURCC('s', 'i', 'd', 'e'), 0 },
	{ "Center View", BUTN, FOURCC('n', 'o', 'n', 'e'), 0 },
	{ "Look Up", BUTN, FOURCC('b', 't', 'l', 'u'), 0x90000000 },
	{ "Look Down", BUTN, FOURCC('b', 't', 'l', 'd'), 0x90000000 },
	{ "Turn Left", BUTN, FOURCC('b', 't', 't', 'l'), 0x90000000 },
	{ "Turn Right", BUTN, FOURCC('b', 't', 't', 'r'), 0x90000000 },
	{ "Run Forward", BUTN, FOURCC('b', 't', 'm', 'f'), 0x90000000 },
	{ "Run Backward", BUTN, FOURCC('b', 't', 'm', 'b'), 0x90000000 },
};
#define UT_NEEDS (sizeof ut / sizeof ut[0])

/* ... and what meets them: a shooter's layout (the axes are left to the sticks' deltas and buttons) */
static const uint8_t ut_controls[UT_NEEDS] = {
	ISP_NONE, ISP_NONE, ISP_RIGHT_X, ISP_RIGHT_Y, ISP_RIGHT_TRIGGER, ISP_LEFT_TRIGGER, ISP_A, ISP_B,
	ISP_LEFT_SHOULDER, ISP_RIGHT_SHOULDER, ISP_X, ISP_Y, ISP_DPAD_LEFT, ISP_DPAD_RIGHT, ISP_DPAD_UP, ISP_DPAD_DOWN,
	ISP_LEFT_X_LEFT, ISP_LEFT_X_RIGHT, ISP_LEFT_STICK, ISP_RIGHT_STICK, ISP_NONE, ISP_NONE, ISP_NONE, ISP_NONE,
	ISP_LEFT_Y_UP, ISP_LEFT_Y_DOWN,
};

static unsigned make_needs(struct isp_need *needs, const struct need_def *defs, unsigned n)
{
	for (unsigned k = 0; k < n; k++) {
		memset(&needs[k], 0, sizeof needs[k]);
		snprintf(needs[k].name, sizeof needs[k].name, "%s", defs[k].name);
		needs[k].kind = defs[k].kind;
		needs[k].label = defs[k].label;
		needs[k].flags = defs[k].flags;
	}
	return n;
}

static void test_autoconfigure(void)
{
	struct isp_need needs[ISP_NEEDS];
	bool used[ISP_NEEDS] = { false };
	unsigned n = make_needs(needs, ut, UT_NEEDS);
	isp_autoconfigure(needs, n, used);
	for (unsigned k = 0; k < n; k++) {
		if (needs[k].control != ut_controls[k])
			fprintf(stderr, "isp: %s: %s, expected %s\n", needs[k].name, isp_control_name(needs[k].control),
				isp_control_name(ut_controls[k]));
		CHECK_EQ(needs[k].control, ut_controls[k]);
		CHECK_EQ(used[k], ut_controls[k] != ISP_NONE);
	}

	/* a flight game's: the stick, the rudder on the right one, the throttle on a trigger; a need another
	 * device took that's to be met by one only, and a utility (the keyboard's), aren't met */
	static const struct need_def flight[] = {
		{ "Roll", AXIS, FOURCC('x', 'a', 'x', 'i'), 0 },
		{ "Pitch", AXIS, FOURCC('y', 'a', 'x', 'i'), 0 },
		{ "Rudder", AXIS, FOURCC('r', 'u', 'd', 'd'), 0 },
		{ "Throttle", AXIS, FOURCC('t', 'h', 'r', 't'), ISP_NEED_AXIS_ASYMMETRIC },
		{ "Brake", AXIS, FOURCC('n', 'o', 'n', 'e'), ISP_NEED_AXIS_ASYMMETRIC },
		{ "Guns", BUTN, FOURCC('f', 'i', 'r', 'e'), 0 },
		{ "Missile", BUTN, FOURCC('n', 'o', 'n', 'e'), 0 },
		{ "Eject", BUTN, FOURCC('n', 'o', 'n', 'e'), ISP_NEED_NO_MULTI_CONFIG },
		{ "Frame rate", BUTN, FOURCC('n', 'o', 'n', 'e'), ISP_NEED_UTILITY },
		{ "Look", ISP_KIND_DPAD, FOURCC('p', 'o', 'v', 'h'), 0 },
		{ "Map", BUTN, FOURCC('n', 'o', 'n', 'e'), ISP_NEED_NO_AUTO_CONFIG },
	};
	static const uint8_t flight_controls[] = { ISP_LEFT_X, ISP_LEFT_Y, ISP_RIGHT_X, ISP_RIGHT_TRIGGER,
						   ISP_LEFT_TRIGGER, ISP_RIGHT_SHOULDER, ISP_A, ISP_NONE, ISP_NONE,
						   ISP_DPAD, ISP_NONE };
	memset(used, 0, sizeof used);
	used[7] = true;
	n = make_needs(needs, flight, sizeof flight / sizeof flight[0]);
	isp_autoconfigure(needs, n, used);
	for (unsigned k = 0; k < n; k++) {
		if (needs[k].control != flight_controls[k])
			fprintf(stderr, "isp: %s: %s, expected %s\n", needs[k].name, isp_control_name(needs[k].control),
				isp_control_name(flight_controls[k]));
		CHECK_EQ(needs[k].control, flight_controls[k]);
	}
	CHECK(used[7] && !used[8]);

	/* what can meet what */
	CHECK(isp_control_meets(ISP_A, BUTN, 0) && isp_control_meets(ISP_LEFT_Y_UP, BUTN, 0));
	CHECK(isp_control_meets(ISP_RIGHT_TRIGGER, BUTN, 0) && isp_control_meets(ISP_RIGHT_TRIGGER, AXIS, 0));
	CHECK(!isp_control_meets(ISP_DPAD, BUTN, 0) && !isp_control_meets(ISP_RIGHT_TRIGGER, DLTA, 0));
	CHECK(isp_control_meets(ISP_MOVEMENT, ISP_KIND_MOVEMENT, 0) && !isp_control_meets(ISP_LEFT_X, ISP_KIND_MOVEMENT, 0));
	CHECK(!strcmp(isp_control_name(ISP_RIGHT_STICK), "Right stick button"));
	CHECK(!strcmp(isp_control_name(ISP_NONE), "None"));
}

/* ---- data ---------------------------------------------------------------------------- */

static void test_data(void)
{
	struct pad_slot p;
	memset(&p, 0, sizeof p);
	p.connected = true;
	int64_t rest = 0;
	CHECK_EQ(isp_data(&p, ISP_A, BUTN, 0, 0, NULL), 0);
	p.down = 1u << PAD_A | 1u << PAD_UP | 1u << PAD_LEFT;
	CHECK_EQ(isp_data(&p, ISP_A, BUTN, 0, 0, NULL), 1);
	CHECK_EQ(isp_data(&p, ISP_DPAD_UP, BUTN, 0, 0, NULL), 1);
	CHECK_EQ(isp_data(&p, ISP_DPAD_DOWN, BUTN, 0, 0, NULL), 0);
	CHECK_EQ(isp_data(&p, ISP_DPAD, ISP_KIND_DPAD, 0, 0, NULL), 2);         /* kISpPadUpLeft */
	p.down = 1u << PAD_DOWN;
	CHECK_EQ(isp_data(&p, ISP_DPAD, ISP_KIND_DPAD, 0, 0, NULL), 7);         /* kISpPadDown */
	p.down = 0;
	CHECK_EQ(isp_data(&p, ISP_DPAD, ISP_KIND_DPAD, 0, 0, NULL), 0);
	CHECK_EQ(isp_data(NULL, ISP_DPAD, ISP_KIND_DPAD, 0, 0, NULL), 0);

	/* axes: left and down the minimum, right and up the maximum; a trigger from 0; as buttons, past half way */
	CHECK_EQ(isp_data(&p, ISP_LEFT_X, AXIS, 0, 0, NULL), 0x80008000u);     /* (the middle, near enough) */
	p.axis[PAD_LEFT_X] = -32768;
	p.axis[PAD_LEFT_Y] = -32768;                            /* (SDL's up) */
	p.axis[PAD_RIGHT_TRIGGER] = 32767;
	p.axis[PAD_LEFT_TRIGGER] = 10000;
	CHECK_EQ(isp_data(&p, ISP_LEFT_X, AXIS, 0, 0, NULL), 0);
	CHECK_EQ(isp_data(&p, ISP_LEFT_Y, AXIS, 0, 0, NULL), 0xFFFFFFFFu);
	CHECK_EQ(isp_data(&p, ISP_RIGHT_TRIGGER, AXIS, 0, 0, NULL), 0xFFFFFFFFu);
	CHECK_EQ(isp_data(&p, ISP_RIGHT_TRIGGER, BUTN, 0, 0, NULL), 1);
	CHECK_EQ(isp_data(&p, ISP_LEFT_TRIGGER, BUTN, 0, 0, NULL), 0);
	CHECK_EQ(isp_data(&p, ISP_LEFT_X_LEFT, BUTN, 0, 0, NULL), 1);
	CHECK_EQ(isp_data(&p, ISP_LEFT_Y_UP, BUTN, 0, 0, NULL), 1);
	CHECK_EQ(isp_data(&p, ISP_LEFT_Y_DOWN, BUTN, 0, 0, NULL), 0);
	CHECK_EQ(isp_data(&p, ISP_MOVEMENT, ISP_KIND_MOVEMENT, 0, 0, NULL), 2);

	/* a delta: the mouse's counts as the right stick moves it, ISP_DELTA_COUNT a count; up positive */
	p.axis[PAD_RIGHT_X] = 32767;
	p.axis[PAD_RIGHT_Y] = 32767;                            /* (down) */
	int64_t sum = 0, sum_y = 0, rest_y = 0;
	for (unsigned k = 0; k < 100; k++) {                    /* a second, 10 ms at a time */
		sum += (int32_t)isp_data(&p, ISP_RIGHT_X, DLTA, HZ / 100, HZ, &rest);
		sum_y += (int32_t)isp_data(&p, ISP_RIGHT_Y, DLTA, HZ / 100, HZ, &rest_y);
	}
	CHECK_EQ(sum, PAD_MOUSE_SPEED * ISP_DELTA_COUNT);
	CHECK_EQ(sum_y, -PAD_MOUSE_SPEED * ISP_DELTA_COUNT);
	p.axis[PAD_RIGHT_X] = 3000;                             /* (in the dead zone) */
	CHECK_EQ(isp_data(&p, ISP_RIGHT_X, DLTA, HZ, HZ, &rest), 0);
}

/* ---- the driver, as InputSprocket drives it ---------------------------------------- */

static struct mem mem;
static struct isp isp;
static struct pad pads;

#define TOC 0x10000u
#define FRAME 0x30000u
#define TVECTORS 0x40000u               /* each import's: TVECTORS + 8 * k */
#define NEEDS 0x50000u                  /* the game's needs, */
#define ELEMENTS 0x58000u               /* ... its virtual elements for them, */
#define USED 0x59000u                   /* ... and which are met */
#define BUF 0x5A000u
#define DIALOG 0x60000u
#define HANDLE 0x61000u                 /* (a handle: its master pointer, then its block) */
#define EVENT 0x62000u
#define DEVICE_REF 0x00D0E000u
#define ELEMENT_REF 0x00E1E000u         /* (+ the element's number) */
#define VIRTUAL 0x00F1E000u             /* (+ the need's number) */

static struct {
	unsigned calls[NDRV_MAX_IMPORTS];
	char log[4096];                 /* the calls, by name */
	char pushed[1024];              /* what was pushed: "element=value ..." */
	unsigned elements;
	uint32_t device_def;
	bool timer;                     /* the Time Manager task is in */
	uint32_t ditl;                  /* the DITL appended */
	unsigned items;
} os;

static void add(char *s, size_t size, const char *fmt, uint32_t a, uint32_t b)
{
	size_t n = strlen(s);
	snprintf(s + n, size - n, fmt, a, b);
}

/* An import's call: its result */
static uint32_t fake(unsigned imp, const uint32_t *a)
{
	const char *name = imp < imports ? import_name[imp] : "?";
	os.calls[imp]++;
	static const char *const quiet[] = { "ISpElement_PushSimpleData", "SetPort", "MoveTo", "LineTo", "TruncString",
					     "DrawString", "TextFont", "TextSize", "EraseRect" };
	bool said = false;
	for (size_t k = 0; k < sizeof quiet / sizeof quiet[0]; k++)
		said |= !strcmp(name, quiet[k]);
	if (!said) {
		size_t n = strlen(os.log);
		snprintf(os.log + n, sizeof os.log - n, "%s ", name);
	}
	if (!strcmp(name, "ISpDevice_New")) {
		os.device_def = a[0];
		mem_w32(&mem, a[3], DEVICE_REF);
	} else if (!strcmp(name, "ISpElement_New")) {
		CHECK_EQ(mem_r32(&mem, a[0]), DEVICE_REF);      /* (each element's device) */
		mem_w32(&mem, a[1], ELEMENT_REF + os.elements++);
	} else if (!strcmp(name, "ISpElement_PushSimpleData")) {
		add(os.pushed, sizeof os.pushed, "%X=%X ", a[0], a[1]);
	} else if (!strcmp(name, "GetCurrentProcess")) {
		mem_w32(&mem, a[0], 0);
		mem_w32(&mem, a[0] + 4, 0x2345);
	} else if (!strcmp(name, "InsTime")) {
		os.timer = true;
	} else if (!strcmp(name, "RmvTime")) {
		os.timer = false;
	} else if (!strcmp(name, "CountDITL")) {
		return os.items;
	} else if (!strcmp(name, "NewHandle")) {
		mem_w32(&mem, HANDLE, HANDLE + 8);
		return HANDLE;
	} else if (!strcmp(name, "AppendDITL")) {
		os.ditl = a[1];
		os.items += mem_r16(&mem, mem_r32(&mem, a[1])) + 1;
	} else if (!strcmp(name, "ShortenDITL")) {
		os.items -= a[1];
	}
	return 0;
}

/* Runs one of the driver's entries to its end, answering its calls; returns its result */
static uint32_t call(unsigned entry, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4)
{
	static uint64_t tb = HZ;
	struct ndrv_call c = { .mem = &mem, .entry = entry, .toc = TOC, .frame = FRAME, .tb = tb };
	uint32_t a[8] = { a0, a1, a2, a3, a4 };
	memcpy(c.arg, a, sizeof a);
	for (unsigned n = 0; n < 10000; n++) {
		isp_call(&isp, &c, &pads);
		if (c.done)
			return c.result;
		unsigned imp = (c.tvector - TVECTORS) / 8;
		CHECK(c.tvector >= TVECTORS && imp < imports);
		uint32_t result = fake(imp, c.out);
		c = (struct ndrv_call){ .mem = &mem, .entry = NDRV_ENTRY_RESUME, .toc = TOC, .frame = FRAME, .tb = tb };
		c.arg[0] = result;
	}
	CHECK(!"the driver's calls ended");
	return 0;
}

static unsigned count(const char *s, const char *what)
{
	unsigned n = 0;
	for (const char *p = strstr(s, what); p; p = strstr(p + 1, what))
		n++;
	return n;
}

static void forget(void)
{
	os.log[0] = os.pushed[0] = '\0';
}

static uint32_t selector(unsigned sel)
{
	return ISP_ENTRY_SELECTOR + sel - 1;
}

/* The needs in guest memory, as a game gives them */
static void put_needs(const struct need_def *defs, unsigned n)
{
	for (unsigned k = 0; k < n; k++) {
		uint32_t at = NEEDS + ISP_NEED_SIZE * k;
		memset(mem_ptr(&mem, at), 0, ISP_NEED_SIZE);
		mem_w8(&mem, at, (uint8_t)strlen(defs[k].name));
		memcpy(mem_ptr(&mem, at + 1), defs[k].name, strlen(defs[k].name));
		mem_w32(&mem, at + 68, defs[k].kind);
		mem_w32(&mem, at + 72, defs[k].label);
		mem_w32(&mem, at + 76, defs[k].flags);
		mem_w32(&mem, ELEMENTS + 4 * k, VIRTUAL + k);
		mem_w8(&mem, USED + k, 0);
	}
}

static void out_key(void *ctx, uint8_t key, bool down)
{
	(void)ctx, (void)key, (void)down;
}

static void out_button(void *ctx, bool down)
{
	(void)ctx, (void)down;
}

static void out_motion(void *ctx, int dx, int dy)
{
	(void)ctx, (void)dx, (void)dy;
}

static void load(void)
{
	isp_init(&isp, HZ);
	memset(mem_ptr(&mem, TOC), 0, 0x8000);
	for (unsigned k = 0; k < imports; k++)
		mem_w32(&mem, TOC + layout.import[k], TVECTORS + 8 * k);
	mem_w32(&mem, TOC + layout.extra, FOURCC('B', 'l', 'I', 's'));
	mem_w32(&mem, TOC + layout.extra + 4, isp.hash);
	memset(&os, 0, sizeof os);
	os.items = 8;
	mem_w8(&mem, 0x910, 18);                                /* (CurApName) */
	memcpy(mem_ptr(&mem, 0x911), "Unreal Tournament!", 18);
}

static void test_driver(void)
{
	pad_init(&pads, &(struct pad_output){ NULL, out_key, out_button, out_motion });
	pad_plug(&pads, 0, "Test Pad");
	load();

	/* loaded: the pad is a device, with its elements, and the task is in */
	CHECK_EQ(call(ISP_ENTRY_INIT, 0, 0, 0, 0, 0), 0);
	CHECK(!strcmp(os.log, "GetCurrentProcess ISpDevice_New " "ISpElement_New ISpElement_New ISpElement_New "
			      "ISpElement_New ISpElement_New ISpElement_New ISpElement_New ISpElement_New "
			      "ISpElement_New ISpElement_New ISpElement_New ISpElement_New ISpElement_New "
			      "ISpElement_New ISpElement_New ISpElement_New ISpElement_New ISpElement_New InsTime "
			      "PrimeTime ") || (fprintf(stderr, "isp: %s\n", os.log), false));
	CHECK_EQ(os.elements, ISP_ELEMENTS);
	CHECK(os.timer);
	CHECK_EQ(mem_r8(&mem, os.device_def), 8);
	CHECK(!memcmp(mem_ptr(&mem, os.device_def + 1), "Test Pad", 8));
	CHECK_EQ(mem_r32(&mem, os.device_def + 64), FOURCC('g', 'm', 'p', 'd'));
	CHECK_EQ(mem_r32(&mem, os.device_def + 72), 1);         /* (permanentID) */
	CHECK(!isp_in_use(&isp, NULL, NULL));
	/* (InputSprocket 1.7 asks again: nothing new, and the driver stays) */
	forget();
	mem_w8(&mem, BUF, 0);
	CHECK_EQ(call(ISP_ENTRY_FIND, BUF, 0, 0, 0, 0), 0);
	CHECK_EQ(mem_r8(&mem, BUF), 1);
	CHECK(!strcmp(os.log, ""));
	mem_w8(&mem, BUF, 0);
	call(ISP_ENTRY_CHECK, BUF, 0, 0, 0, 0);
	CHECK_EQ(mem_r8(&mem, BUF), 1);

	/* the meta handler: a function for each selector */
	uint32_t init = call(ISP_ENTRY_META, 0, ISP_SEL_INIT, 0, 0, 0);
	CHECK_EQ(init, TOC + layout.entry[selector(ISP_SEL_INIT)]);
	CHECK_EQ(call(ISP_ENTRY_META, 0, 13, 0, 0, 0), TOC + layout.entry[selector(13)]);
	CHECK_EQ(call(ISP_ENTRY_META, 0, 23, 0, 0, 0), 0);

	/* the game's needs: those the pad meets are used, and it's in use */
	put_needs(ut, UT_NEEDS);
	mem_w8(&mem, USED + 4, 1);                              /* (the mouse's already) */
	CHECK_EQ(call(selector(ISP_SEL_INIT), 0, UT_NEEDS, NEEDS, ELEMENTS, USED), 0);
	for (unsigned k = 0; k < UT_NEEDS; k++)
		CHECK_EQ(mem_r8(&mem, USED + k), ut_controls[k] != ISP_NONE || k == 4);
	uint32_t psn[2];
	const char *game = NULL;
	CHECK(isp_in_use(&isp, psn, &game));
	CHECK(psn[1] == 0x2345 && game && !strcmp(game, "Unreal Tournament!"));
	uint32_t taken = isp_taken(&isp, 0);
	CHECK(taken & 1u << PAD_A && taken & 1u << PAD_STICK_UP && taken & PAD_TAKEN_RIGHT_STICK);
	CHECK(!(taken & 1u << PAD_START) && !(taken & 1u << PAD_BACK));
	CHECK_EQ(isp_taken(&isp, 1), 0);

	/* the task: everything at first, then what changes, to the elements and the needs met */
	forget();
	call(ISP_ENTRY_TIMER, 0, 0, 0, 0, 0);
	CHECK_EQ(count(os.pushed, "E1E0"), ISP_ELEMENTS);
	CHECK(strstr(os.log, "PrimeTime") != NULL);
	forget();
	call(ISP_ENTRY_TIMER, 0, 0, 0, 0, 0);
	CHECK(!strcmp(os.pushed, ""));
	pad_button(&pads, 0, PAD_A, true);
	call(ISP_ENTRY_TIMER, 0, 0, 0, 0, 0);
	CHECK(!strcmp(os.pushed, "E1E000=1 F1E006=1 ") || (fprintf(stderr, "isp: %s\n", os.pushed), false));
	forget();
	call(ISP_ENTRY_TICKLE, 0, 0, 0, 0, 0);                  /* (the game's ISpTickle: the same) */
	CHECK(!strcmp(os.pushed, ""));
	pad_button(&pads, 0, PAD_A, false);
	call(selector(15), 0, 0, 0, 0, 0);
	CHECK(!strcmp(os.pushed, "E1E000=0 F1E006=0 ") || (fprintf(stderr, "isp: %s\n", os.pushed), false));

	/* its state, as InputSprocket keeps it for the game: and as it comes back */
	mem_w32(&mem, BUF + 0x800, 0);
	CHECK_EQ(call(selector(10), 0, 0, 0, BUF + 0x800, 0), 0);
	uint32_t len = mem_r32(&mem, BUF + 0x800);
	CHECK_EQ(len, 6 + UT_NEEDS);
	CHECK_EQ(call(selector(10), 0, 0x800, BUF, BUF + 0x800, 0), 0);
	CHECK_EQ(mem_r32(&mem, BUF), FOURCC('B', 'l', 'G', 'p'));
	CHECK_EQ(mem_r8(&mem, BUF + 6 + 6), ISP_A);             /* (Jump) */
	mem_w8(&mem, BUF + 6 + 6, ISP_B);
	mem_w8(&mem, BUF + 6 + 7, ISP_DPAD);                    /* (not a button: ignored) */
	call(selector(11), 0, len, BUF, 0, 0);
	forget();
	pad_button(&pads, 0, PAD_B, true);
	call(ISP_ENTRY_TIMER, 0, 0, 0, 0, 0);
	CHECK(strstr(os.pushed, "F1E006=1") && strstr(os.pushed, "F1E007=1"));  /* (Jump, and Crouch as it was) */
	pad_button(&pads, 0, PAD_B, false);

	/* its panel: a user item over the rect it's given, drawn; gone as it's hidden */
	mem_w16(&mem, BUF + 0x900, 50);
	mem_w16(&mem, BUF + 0x902, 120);
	mem_w16(&mem, BUF + 0x904, 300);
	mem_w16(&mem, BUF + 0x906, 560);
	forget();
	CHECK_EQ(call(selector(7), 0, UT_NEEDS, NEEDS, 0, 0), 0);       /* (BeginConfiguration) */
	call(selector(5), 0, DIALOG, 9, BUF + 0x900, 0);                /* (Show) */
	CHECK(strstr(os.log, "CountDITL NewHandle AppendDITL DisposeHandle SetDialogItem") != NULL);
	CHECK_EQ(os.items, 9);
	CHECK_EQ(mem_r16(&mem, HANDLE + 8 + 6), 50);            /* (the item's rect) */
	CHECK_EQ(mem_r16(&mem, HANDLE + 8 + 12), 560);
	CHECK_EQ(mem_r8(&mem, HANDLE + 8 + 14), 0);             /* (a user item, enabled) */
	CHECK(os.calls[import_named("TruncString")] >= 2 * (UT_NEEDS - 0));
	/* (a click outside its boxes isn't its) */
	mem_w16(&mem, EVENT, 1);
	mem_w32(&mem, EVENT + 10, 0x00300010);
	mem_w8(&mem, BUF + 0xA00, 0xFF);
	call(selector(4), 0, EVENT, BUF + 0xA00, 0, 0);
	CHECK_EQ(mem_r8(&mem, BUF + 0xA00), 0);
	mem_w8(&mem, BUF + 0xA00, 1);
	call(selector(12), 0, BUF + 0xA00, 0, 0, 0);            /* (Dirty: nothing changed yet) */
	CHECK_EQ(mem_r8(&mem, BUF + 0xA00), 0);
	call(selector(6), 0, 0, 0, 0, 0);                       /* (Hide) */
	CHECK_EQ(os.items, 8);
	call(selector(8), 0, 1, 0, 0, 0);                       /* (EndConfiguration) */

	/* inactive (the game's in the background): not in use */
	call(selector(13), 0, 0, 0, 0, 0);
	CHECK(!isp_in_use(&isp, NULL, NULL));
	CHECK_EQ(isp_taken(&isp, 0), 0);
	call(selector(13), 0, 1, 0, 0, 0);
	CHECK(isp_in_use(&isp, NULL, NULL));
	call(selector(2), 0, 0, 0, 0, 0);                       /* (Stop) */
	CHECK(!isp_in_use(&isp, NULL, NULL));

	/* unloaded: the task goes */
	forget();
	call(ISP_ENTRY_TERM, 0, 0, 0, 0, 0);
	CHECK(!os.timer);
	CHECK(!strcmp(os.log, "RmvTime "));
	forget();
	call(ISP_ENTRY_TIMER, 0, 0, 0, 0, 0);                   /* (a late one does nothing) */
	CHECK(!strcmp(os.log, ""));

	/* a copy of another version's isn't run */
	load();
	mem_w32(&mem, TOC + layout.extra + 4, isp.hash ^ 1);
	CHECK_EQ(call(ISP_ENTRY_INIT, 0, 0, 0, 0, 0), 0);
	CHECK(!strcmp(os.log, ""));
	CHECK(isp.stale_said);
}

void test_isp(void)
{
	test_fragment();
	test_autoconfigure();
	test_data();
	if (mem_init(&mem)) {
		CHECK(!"mem_init");
		return;
	}
	mem_map(&mem, 0, 0x100000, MEM_RW);
	test_driver();
	mem_free(&mem);
}
