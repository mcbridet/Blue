/*
 * Game controllers as the keyboard and mouse (pad.h): Blue's mapping, a
 * Pippin title's AppleJack mapping from its 'pipp' resource, the sticks and
 * triggers, keys held by more than one control, and the right stick's mouse.
 */
#include <string.h>

#include "../src/be.h"
#include "../src/pad.h"
#include "test.h"

static struct {
	char log[2048];                 /* what went to the keyboard and mouse: "+24 -24 M+ M- (3,-1)" */
	int dx, dy;
} out;

static void say(const char *what)
{
	size_t n = strlen(out.log);
	snprintf(out.log + n, sizeof out.log - n, "%s%s", n ? " " : "", what);
}

static void key(void *ctx, uint8_t adb, bool down)
{
	(void)ctx;
	char s[8];
	snprintf(s, sizeof s, "%c%02X", down ? '+' : '-', adb);
	say(s);
}

static void button(void *ctx, bool down)
{
	(void)ctx;
	say(down ? "M+" : "M-");
}

static void motion(void *ctx, int dx, int dy)
{
	(void)ctx;
	out.dx += dx;
	out.dy += dy;
}

/* What happened since the last look, which it then forgets */
static bool saw(const char *expected)
{
	bool same = !strcmp(out.log, expected);
	if (!same)
		fprintf(stderr, "pad: saw \"%s\", expected \"%s\"\n", out.log, expected);
	out.log[0] = '\0';
	return same;
}

static void start(struct pad *p)
{
	memset(&out, 0, sizeof out);
	pad_init(p, &(struct pad_output){ NULL, key, button, motion });
	CHECK_EQ(pad_plug(p, -1, "Test Pad"), 0);
}

/* Super Marathon's gameplay mapping (its 'pipp' 1001), as AJGlobalData: a SwitchData a switch */
static const uint32_t marathon_game[PAD_AJ_SWITCHES] = {
	0x04010C71, 0x06003B09, 0x04002370, 0, 0, 0x00011F6F, 0x0001067A, 0x0600371C,  /* Cmd-Q, Control, P */
	0x06003A7A, 0x04003120, 0x04001F6F, 0x0400286B, 0x0400293B, 0x0400256C,         /* Option, Space, O K ; L */
	0x06003837, 0x04003009, 0x04000778, 0x0400067A,                                 /* Shift, Tab, X, Z */
};
/* ... and its menus' (1000): the mouse button locked, a double-click, the arrows */
static const uint32_t marathon_menus[PAD_AJ_SWITCHES] = {
	0x04010C71, 0x03007B1C, 0x02007C1D, 0, 0, 0x00011F6F, 0x0001067A, 0x0600371C,
	0x04000C71, 0x04000F72, 0x04007E1E, 0x04007B1C, 0x04007C1D, 0x04007D1F, 0x04000E65, 0x04040D77,
	0x01000000, 0x01FFFFFF,
};

static void make_pipp(uint8_t res[PAD_AJ_SIZE], const uint32_t map[PAD_AJ_SWITCHES])
{
	memset(res, 0, PAD_AJ_SIZE);
	put_be32(res, PAD_AJ_SIGNATURE);
	put_be32(res + 4, 0x40);
	put_be32(res + PAD_AJ_STATES, 0x12345678);              /* (whatever the editor left there) */
	for (unsigned k = 0; k < PAD_AJ_SWITCHES; k++)
		put_be32(res + PAD_AJ_MAP + 4 * k, map[k]);
	put_be32(res + 84, 0x00812340);
	put_be32(res + 88, 0x40812344);
}

static void test_pipp(void)
{
	uint8_t res[PAD_AJ_SIZE];
	struct pad_switch map[PAD_AJ_SWITCHES];
	make_pipp(res, marathon_game);
	CHECK(pad_pipp(res, sizeof res, map));
	CHECK(map[10].function == PAD_DO_KEY && map[10].key == 0x1F && map[10].chr == 'o');
	CHECK(map[8].function == PAD_DO_MODIFIER && map[8].key == 0x3A);
	CHECK(map[0].function == PAD_DO_KEY && map[0].modifiers == PAD_MOD_COMMAND && map[0].key == 0x0C);
	CHECK(pad_pipp(res, PAD_AJ_MAP + 4 * PAD_AJ_SWITCHES, map));   /* (without the pointers after) */
	CHECK(!pad_pipp(res, PAD_AJ_MAP + 4 * PAD_AJ_SWITCHES - 1, map));
	res[0] = 'P';
	CHECK(!pad_pipp(res, sizeof res, map));

	char text[48];
	make_pipp(res, marathon_game);
	pad_pipp(res, sizeof res, map);
	pad_switch_text(&map[0], text, sizeof text);
	CHECK(!strcmp(text, "Command-Q"));
	pad_switch_text(&map[1], text, sizeof text);
	CHECK(!strcmp(text, "Control"));
	pad_switch_text(&map[9], text, sizeof text);
	CHECK(!strcmp(text, "Space"));
	pad_switch_text(&map[12], text, sizeof text);
	CHECK(!strcmp(text, ";"));
	make_pipp(res, marathon_menus);
	pad_pipp(res, sizeof res, map);
	pad_switch_text(&map[1], text, sizeof text);
	CHECK(!strcmp(text, "the mouse button, locked"));
	pad_switch_text(&map[10], text, sizeof text);
	CHECK(!strcmp(text, "Up Arrow"));
	pad_switch_text(&map[17], text, sizeof text);
	CHECK(!strcmp(text, "the mouse button"));

	/* virtual key codes to the ADB keyboard's: the arrows and Control differ */
	CHECK_EQ(pad_adb_key(0x7E), 0x3E);
	CHECK_EQ(pad_adb_key(0x7B), 0x3B);
	CHECK_EQ(pad_adb_key(0x3B), 0x36);
	CHECK_EQ(pad_adb_key(0x36), 0x36);
	CHECK_EQ(pad_adb_key(0x1F), 0x1F);
	CHECK_EQ(pad_adb_key(0x39), 0xFF);
	CHECK_EQ(pad_adb_key(0x7F), 0xFF);

	CHECK_EQ(pad_button_named("a"), PAD_A);
	CHECK_EQ(pad_button_named("dpleft"), PAD_LEFT);
	CHECK_EQ(pad_button_named("rightshoulder"), PAD_RIGHT_SHOULDER);
	CHECK_EQ(pad_button_named("A"), -1);
	CHECK_EQ(pad_axis_named("righttrigger"), PAD_RIGHT_TRIGGER);
	CHECK_EQ(pad_axis_named("lefty"), PAD_LEFT_Y);
	CHECK_EQ(pad_axis_named("z"), -1);
}

/* Blue's own mapping */
static void test_default(void)
{
	struct pad p;
	start(&p);
	pad_button(&p, 0, PAD_A, true);
	pad_button(&p, 0, PAD_A, true);                         /* (no change) */
	pad_button(&p, 0, PAD_A, false);
	CHECK(saw("+24 -24"));                                  /* Return */
	pad_button(&p, 0, PAD_B, true);
	pad_button(&p, 0, PAD_B, false);
	CHECK(saw("+37 +2F -2F -37"));                          /* Command-period */
	pad_button(&p, 0, PAD_X, true);
	pad_button(&p, 0, PAD_Y, true);
	pad_button(&p, 0, PAD_START, true);
	CHECK(saw("+31 +30 +35"));                              /* Space, Tab, Esc */
	pad_button(&p, 0, PAD_LEFT_SHOULDER, true);
	pad_button(&p, 0, PAD_RIGHT_SHOULDER, true);
	pad_button(&p, 0, PAD_BACK, true);
	CHECK(saw("+38 +3A +37"));                              /* Shift, Option, Command */
	pad_button(&p, 0, PAD_GUIDE, true);
	CHECK(saw(""));
	pad_unplug(&p, 0);                                      /* (what it held goes up) */
	CHECK(saw("-31 -30 -37 -35 -38 -3A"));
	CHECK_EQ(pad_count(&p), 0);

	/* the D-pad and the left stick, the arrows: up held by both goes up when both have let go */
	CHECK_EQ(pad_plug(&p, -1, NULL), 0);
	CHECK(!strcmp(p.slot[0].name, "Game controller"));
	pad_button(&p, 0, PAD_UP, true);
	pad_axis(&p, 0, PAD_LEFT_Y, -20000);
	CHECK(saw("+3E"));
	pad_button(&p, 0, PAD_UP, false);
	CHECK(saw(""));
	pad_axis(&p, 0, PAD_LEFT_Y, -13000);                    /* (still pushed, past where it lets go) */
	CHECK(saw(""));
	pad_axis(&p, 0, PAD_LEFT_Y, -11000);
	CHECK(saw("-3E"));
	pad_axis(&p, 0, PAD_LEFT_Y, -15000);                    /* (not yet pressed again) */
	CHECK(saw(""));
	pad_axis(&p, 0, PAD_LEFT_X, 32767);
	pad_axis(&p, 0, PAD_LEFT_X, -32768);
	pad_axis(&p, 0, PAD_LEFT_Y, 30000);
	CHECK(saw("+3C -3C +3B +3D"));
	pad_axis(&p, 0, PAD_LEFT_X, 0);
	pad_axis(&p, 0, PAD_LEFT_Y, 0);
	CHECK(saw("-3B -3D"));

	/* the triggers: the mouse button, the left one with Control; held by both, up when both let go */
	pad_axis(&p, 0, PAD_RIGHT_TRIGGER, 32767);
	CHECK(saw("M+"));
	pad_axis(&p, 0, PAD_LEFT_TRIGGER, 20000);
	CHECK(saw("+36"));
	pad_axis(&p, 0, PAD_RIGHT_TRIGGER, 0);
	CHECK(saw(""));
	pad_axis(&p, 0, PAD_LEFT_TRIGGER, 0);
	CHECK(saw("M- -36"));
	pad_button(&p, 0, PAD_RIGHT_STICK, true);
	pad_button(&p, 0, PAD_RIGHT_STICK, false);
	CHECK(saw("M+ M-"));

	/* two pads */
	CHECK_EQ(pad_plug(&p, -1, "Second"), 1);
	pad_button(&p, 0, PAD_X, true);
	pad_button(&p, 1, PAD_X, true);
	pad_button(&p, 0, PAD_X, false);
	CHECK(saw("+31"));
	pad_unplug(&p, 1);
	CHECK(saw("-31"));
	pad_button(&p, 1, PAD_X, true);                         /* (not plugged in) */
	pad_button(&p, 0, PAD_BUTTONS, true);
	pad_axis(&p, 0, PAD_AXES, 32767);
	CHECK(saw(""));
	for (int k = 0; k < PAD_MAX; k++)
		CHECK_EQ(pad_plug(&p, -1, "More"), k == 0 ? 1 : k == 1 ? 2 : k == 2 ? 3 : -1);
	CHECK_EQ(pad_plug(&p, -1, "Too many"), -1);

	/* quiet (the menu is open), and off: presses do nothing, and their releases nothing either */
	p.quiet = true;
	pad_button(&p, 0, PAD_A, true);
	p.quiet = false;
	pad_button(&p, 0, PAD_A, false);
	p.mapping = PAD_MAP_OFF;
	pad_button(&p, 0, PAD_A, true);
	pad_button(&p, 0, PAD_A, false);
	CHECK(saw(""));
	/* a press undone as it was made, whatever the mapping is by the release */
	p.mapping = PAD_MAP_DEFAULT;
	pad_button(&p, 0, PAD_A, true);
	p.mapping = PAD_MAP_OFF;
	pad_button(&p, 0, PAD_A, false);
	CHECK(saw("+24 -24"));
}

/* A Pippin title's: the AppleJack's switches */
static void test_applejack(void)
{
	struct pad p;
	uint8_t res[PAD_AJ_SIZE];
	start(&p);
	make_pipp(res, marathon_game);
	pad_pipp(res, sizeof res, p.pippin[0]);
	p.mapping = PAD_MAP_PIPPIN;
	static const struct { unsigned button; const char *saw; } game[] = {
		{ PAD_UP, "+1F -1F" }, { PAD_LEFT, "+28 -28" }, { PAD_RIGHT, "+29 -29" }, { PAD_DOWN, "+25 -25" },
		{ PAD_A, "+38 -38" },                           /* red: Shift */
		{ PAD_B, "+30 -30" },                           /* green: Tab */
		{ PAD_X, "+31 -31" },                           /* yellow: Space */
		{ PAD_Y, "+3A -3A" },                           /* blue: Option */
		{ PAD_LEFT_SHOULDER, "+06 -06" },               /* left fire: Z */
		{ PAD_RIGHT_SHOULDER, "+07 -07" },              /* right fire: X */
		{ PAD_BACK, "+37 +0C -0C -37" },                /* left system button: Command-Q */
		{ PAD_GUIDE, "+36 -36" },                       /* middle: Control */
		{ PAD_START, "+23 -23" },                       /* right: P */
		{ PAD_LEFT_STICK, "" }, { PAD_RIGHT_STICK, "" }, { PAD_PADDLE1, "" },
	};
	for (size_t k = 0; k < sizeof game / sizeof game[0]; k++) {
		pad_button(&p, 0, game[k].button, true);
		CHECK_EQ(pad_applejack(&p, 0), pad_applejack_switch(game[k].button) >= 0 ?
			 1u << pad_applejack_switch(game[k].button) : 0);
		pad_button(&p, 0, game[k].button, false);
		CHECK(saw(game[k].saw));
		CHECK_EQ(pad_applejack(&p, 0), 0);
	}
	pad_axis(&p, 0, PAD_LEFT_X, -32768);
	pad_axis(&p, 0, PAD_RIGHT_TRIGGER, 32767);
	CHECK_EQ(pad_applejack(&p, 0), 1u << 11 | 1u << 16);
	CHECK(saw("+28 +07"));
	pad_axis(&p, 0, PAD_LEFT_X, 0);
	pad_axis(&p, 0, PAD_RIGHT_TRIGGER, 0);
	CHECK(saw("-28 -07"));

	/* the menus': the mouse button, locked and unlocked; a double-click; a key with caps lock among its modifiers */
	make_pipp(res, marathon_menus);
	pad_pipp(res, sizeof res, p.pippin[0]);
	pad_button(&p, 0, PAD_RIGHT_SHOULDER, true);
	pad_button(&p, 0, PAD_RIGHT_SHOULDER, false);
	CHECK(saw("M+ M-"));
	pad_button(&p, 0, PAD_GUIDE, true);
	pad_button(&p, 0, PAD_GUIDE, false);
	CHECK(saw("M+"));
	pad_button(&p, 0, PAD_LEFT_SHOULDER, true);             /* (held by the lock too) */
	pad_button(&p, 0, PAD_LEFT_SHOULDER, false);
	CHECK(saw(""));
	pad_button(&p, 0, PAD_GUIDE, true);
	CHECK(saw("M-"));
	pad_button(&p, 0, PAD_START, true);
	CHECK(saw("M+ M- M+ M-"));
	pad_button(&p, 0, PAD_B, true);
	CHECK(saw("+0D"));                                      /* green: W (caps lock left alone) */

	/* the game's mapping comes in while a button's down: it goes up as it went down */
	make_pipp(res, marathon_game);
	pad_pipp(res, sizeof res, p.pippin[0]);
	pad_button(&p, 0, PAD_B, false);
	CHECK(saw("-0D"));
	/* the driver's own */
	memcpy(p.pippin[0], pad_applejack_defaults, sizeof p.pippin[0]);
	pad_button(&p, 0, PAD_Y, true);
	pad_button(&p, 0, PAD_X, true);
	CHECK(saw("+6B +26"));                                  /* F14, J */
}

/* The right stick: the mouse, faster the further it's pushed, past a dead zone */
static void test_mouse(void)
{
	struct pad p;
	start(&p);
	const uint64_t hz = 1000;
	pad_tick(&p, 1000, hz);
	pad_axis(&p, 0, PAD_RIGHT_X, 32767);
	for (uint64_t t = 1001; t <= 2000; t++)                 /* a second, a millisecond at a time */
		pad_tick(&p, t, hz);
	CHECK_EQ(out.dx, PAD_MOUSE_SPEED);
	CHECK_EQ(out.dy, 0);
	out.dx = 0;
	pad_axis(&p, 0, PAD_RIGHT_X, 0);
	pad_axis(&p, 0, PAD_RIGHT_Y, -(5000 + (32767 - 5000) / 2));    /* half way past the dead zone: a quarter */
	for (uint64_t t = 2001; t <= 4000; t++)
		pad_tick(&p, t, hz);
	CHECK(out.dy <= -PAD_MOUSE_SPEED / 2 + 1 && out.dy >= -PAD_MOUSE_SPEED / 2 - 1);
	out.dy = 0;
	pad_axis(&p, 0, PAD_RIGHT_Y, 4000);                     /* in the dead zone */
	pad_tick(&p, 5000, hz);
	pad_tick(&p, 5050, hz);
	CHECK(out.dx == 0 && out.dy == 0);
	pad_axis(&p, 0, PAD_RIGHT_X, -32768);                   /* a long wait isn't a long way */
	pad_tick(&p, 60000, hz);
	CHECK_EQ(out.dx, -PAD_MOUSE_SPEED / 10);
	out.dx = 0;
	p.mapping = PAD_MAP_OFF;
	pad_tick(&p, 61000, hz);
	CHECK_EQ(out.dx, 0);
	CHECK(saw(""));
}

/* Controls a game reads itself (through InputSprocket) do nothing; the rest keep the mapping */
static void test_taken(void)
{
	struct pad p;
	start(&p);
	p.slot[0].taken = 1u << PAD_A | 1u << PAD_STICK_UP | PAD_TAKEN_RIGHT_STICK;
	pad_button(&p, 0, PAD_A, true);
	pad_button(&p, 0, PAD_START, true);
	pad_axis(&p, 0, PAD_LEFT_Y, -32768);
	pad_axis(&p, 0, PAD_LEFT_X, 32767);
	CHECK(saw("+35 +3C"));                                  /* Esc, the right arrow */
	pad_tick(&p, 1000, 1000);
	pad_axis(&p, 0, PAD_RIGHT_X, 32767);
	pad_tick(&p, 2000, 1000);
	CHECK_EQ(out.dx, 0);
	p.slot[0].taken = 0;                                    /* (what went down while taken comes up as nothing) */
	pad_button(&p, 0, PAD_A, false);
	pad_button(&p, 0, PAD_START, false);
	CHECK(saw("-35"));
	pad_tick(&p, 2100, 1000);
	CHECK_EQ(out.dx, PAD_MOUSE_SPEED / 10);

	/* the right stick's speed, as a Fixed */
	CHECK_EQ(pad_stick_speed(32767), PAD_MOUSE_SPEED * 65536);
	CHECK_EQ(pad_stick_speed(-32768), -PAD_MOUSE_SPEED * 65536);
	CHECK_EQ(pad_stick_speed(5000), 0);
	int32_t half = pad_stick_speed(5000 + (32767 - 5000) / 2);      /* (half way past the dead zone: a quarter) */
	CHECK(half > PAD_MOUSE_SPEED * 65536 / 4 / 1000 * 999 && half <= PAD_MOUSE_SPEED * 65536 / 4);
}

/* The Mac restarted: what was held is forgotten, not let go */
static void test_forget(void)
{
	struct pad p;
	start(&p);
	pad_button(&p, 0, PAD_A, true);
	pad_axis(&p, 0, PAD_RIGHT_TRIGGER, 32767);
	CHECK(saw("+24 M+"));
	pad_forget(&p);
	pad_button(&p, 0, PAD_A, false);
	pad_axis(&p, 0, PAD_RIGHT_TRIGGER, 0);
	CHECK(saw(""));
	pad_button(&p, 0, PAD_A, true);
	CHECK(saw("+24"));
}

/* Two AppleJacks: the second pad plugged in is the second, by its own mapping and with its own switches */
static void test_two_applejacks(void)
{
	struct pad p;
	uint8_t res[PAD_AJ_SIZE];
	start(&p);
	CHECK_EQ(pad_plug(&p, -1, "Second"), 1);
	make_pipp(res, marathon_game);
	pad_pipp(res, sizeof res, p.pippin[0]);
	make_pipp(res, marathon_menus);
	pad_pipp(res, sizeof res, p.pippin[1]);
	p.mapping = PAD_MAP_PIPPIN;
	/* (one AppleJack: both pads are it) */
	CHECK(pad_applejack_of(&p, 0) == 0 && pad_applejack_of(&p, 1) == 0);
	pad_button(&p, 1, PAD_UP, true);
	CHECK_EQ(pad_applejack(&p, 0), 1u << 10);
	CHECK_EQ(pad_applejack(&p, 1), 0);
	pad_button(&p, 1, PAD_UP, false);
	CHECK(saw("+1F -1F"));                                  /* (the first's mapping: O) */
	p.applejacks = 2;
	CHECK(pad_applejack_of(&p, 0) == 0 && pad_applejack_of(&p, 1) == 1);
	pad_button(&p, 1, PAD_UP, true);
	pad_button(&p, 0, PAD_A, true);
	CHECK_EQ(pad_applejack(&p, 0), 1u << 14);
	CHECK_EQ(pad_applejack(&p, 1), 1u << 10);
	CHECK(saw("+3E +38"));                                  /* the second's up arrow, the first's Shift */
	pad_button(&p, 1, PAD_UP, false);
	pad_button(&p, 0, PAD_A, false);
	CHECK(saw("-3E -38"));
	/* the first unplugged: the one left is the first; plugged in again, it takes the first slot */
	pad_unplug(&p, 0);
	CHECK_EQ(pad_applejack_of(&p, 1), 0);
	CHECK_EQ(pad_plug(&p, -1, "Again"), 0);
	CHECK(pad_applejack_of(&p, 0) == 0 && pad_applejack_of(&p, 1) == 1);
	/* a third: the first AppleJack too */
	CHECK_EQ(pad_plug(&p, -1, "Third"), 2);
	CHECK_EQ(pad_applejack_of(&p, 2), 0);
	pad_axis(&p, 2, PAD_LEFT_X, -32768);
	CHECK_EQ(pad_applejack(&p, 0), 1u << 11);
	CHECK_EQ(pad_applejack(&p, 1), 0);
	CHECK(saw("+28"));
}

/* Blue's mapping, the user's: keys with modifiers, clicks, sticks as the mouse or as keys */
static void test_own_map(void)
{
	struct pad p;
	start(&p);
	CHECK_EQ(pad_plug(&p, -1, "Second"), 1);
	struct pad_map map = pad_default_map;
	CHECK(pad_action_parse("Command-Shift-Z", &map.control[PAD_A]));
	CHECK(pad_action_parse("control-click", &map.control[PAD_B]));
	CHECK(pad_action_parse("Double-Click", &map.control[PAD_X]));
	CHECK(pad_action_parse("Option", &map.control[PAD_Y]));
	CHECK(pad_action_parse("Nothing", &map.control[PAD_START]));
	CHECK(pad_action_parse("W", &map.control[PAD_RSTICK_UP]));
	CHECK(pad_action_parse("Page Down", &map.control[PAD_RSTICK_DOWN]));
	map.stick[0] = PAD_STICK_MOUSE;
	map.stick[1] = PAD_STICK_KEYS;
	pad_set_map(&p, 0, &map);
	pad_button(&p, 0, PAD_A, true);
	pad_button(&p, 0, PAD_A, false);
	CHECK(saw("+37 +38 +06 -06 -38 -37"));
	pad_button(&p, 0, PAD_B, true);
	pad_button(&p, 0, PAD_B, false);
	CHECK(saw("+36 M+ M- -36"));
	pad_button(&p, 0, PAD_X, true);
	pad_button(&p, 0, PAD_X, false);
	CHECK(saw("M+ M- M+ M-"));
	pad_button(&p, 0, PAD_Y, true);
	pad_button(&p, 0, PAD_START, true);
	CHECK(saw("+3A"));
	/* (the other pad keeps Blue's default) */
	pad_button(&p, 1, PAD_START, true);
	CHECK(saw("+35"));
	/* the right stick as keys, the left the mouse */
	pad_axis(&p, 0, PAD_RIGHT_Y, -32768);
	pad_axis(&p, 0, PAD_RIGHT_Y, 32767);
	pad_axis(&p, 0, PAD_RIGHT_Y, 0);
	CHECK(saw("+0D -0D +79 -79"));
	pad_tick(&p, 1000, 1000);
	pad_axis(&p, 0, PAD_LEFT_X, 32767);
	pad_axis(&p, 0, PAD_RIGHT_X, 32767);
	pad_tick(&p, 1100, 1000);
	CHECK(saw("+3C"));                                      /* (the left stick's way: nothing; the right's, its arrow) */
	CHECK(out.dx == PAD_MOUSE_SPEED / 10 && out.dy == 0);
	/* a mapping changed while a control's down: it goes up as it went down */
	pad_set_map(&p, 0, NULL);
	pad_button(&p, 0, PAD_Y, false);
	CHECK(saw("-3A"));
	pad_button(&p, 0, PAD_Y, true);
	CHECK(saw("+30"));
	/* both sticks the mouse, on a Pippin title's AppleJack: the right one only, its trackball */
	out.dx = 0;
	map.stick[1] = PAD_STICK_MOUSE;
	pad_set_map(&p, 0, &map);
	p.mapping = PAD_MAP_PIPPIN;
	pad_tick(&p, 1200, 1000);
	CHECK_EQ(out.dx, PAD_MOUSE_SPEED / 10);
	out.dx = 0;
	p.mapping = PAD_MAP_DEFAULT;
	pad_tick(&p, 1300, 1000);
	CHECK_EQ(out.dx, 2 * PAD_MOUSE_SPEED / 10);
	/* ... and the left one taken by a game (through InputSprocket) */
	out.dx = 0;
	p.slot[0].taken = 1u << PAD_STICK_LEFT;
	pad_tick(&p, 1400, 1000);
	CHECK_EQ(out.dx, PAD_MOUSE_SPEED / 10);
}

/* What a control does, by name: as the settings file has it */
static void test_action_names(void)
{
	static const struct { const char *text, *name; } names[] = {
		{ "Return", "Return" }, { "command-q", "Command-Q" }, { "Shift-Command-Z", "Command-Shift-Z" },
		{ "Command-.", "Command-Period" }, { "Command--", "Command-Minus" }, { "Up Arrow", "Up Arrow" },
		{ "F12", "F12" }, { "Keypad 5", "Keypad 5" }, { "Control", "Control" }, { "click", "Click" },
		{ "Control-Option-Click", "Option-Control-Click" }, { "Double-Click", "Double-Click" },
		{ "Click Lock", "Click Lock" }, { "Nothing", "Nothing" }, { " Esc ", "Esc" }, { ";", "Semicolon" },
		{ "Forward Delete", "Forward Delete" },
	};
	for (size_t k = 0; k < sizeof names / sizeof names[0]; k++) {
		struct pad_switch a;
		char text[64];
		CHECK(pad_action_parse(names[k].text, &a));
		pad_action_name(&a, text, sizeof text);
		if (strcmp(text, names[k].name))
			fprintf(stderr, "pad: \"%s\" is \"%s\", expected \"%s\"\n", names[k].text, text, names[k].name);
		CHECK(!strcmp(text, names[k].name));
	}
	static const char *const bad[] = { "", "Shift-Shift", "Command-Nothing", "Hyper-Q", "Q Q", "Control-Double-Click",
					   "Caps Lock" };
	for (size_t k = 0; k < sizeof bad / sizeof bad[0]; k++) {
		struct pad_switch a;
		CHECK(!pad_action_parse(bad[k], &a));
	}
	/* Blue's default, each control by name and back */
	for (unsigned c = 0; c < PAD_CONTROLS; c++) {
		struct pad_switch a;
		char text[64];
		CHECK_EQ(pad_control_named(pad_control_name(c)), c);
		pad_action_name(&pad_default_map.control[c], text, sizeof text);
		CHECK(pad_action_parse(text, &a));
		CHECK(a.function == pad_default_map.control[c].function && a.key == pad_default_map.control[c].key &&
		      a.modifiers == pad_default_map.control[c].modifiers);
	}
	CHECK_EQ(pad_control_named("rightstickleft"), PAD_RSTICK_LEFT);
	CHECK_EQ(pad_control_named("lefttrigger"), PAD_LT);
	CHECK_EQ(pad_control_named("leftx"), -1);
	/* keys from the keyboard, as the menu takes them */
	CHECK_EQ(pad_virtual_key(0x3E), 0x7E);
	CHECK_EQ(pad_virtual_key(0x36), 0x3B);
	CHECK_EQ(pad_virtual_key(0x0C), 0x0C);
	CHECK_EQ(pad_virtual_key(0x39), 0xFF);
	struct pad_switch a = pad_key_action(0x0C, PAD_MOD_COMMAND | PAD_MOD_CAPS_LOCK);
	CHECK(a.function == PAD_DO_KEY && a.modifiers == PAD_MOD_COMMAND && a.chr == 'q');
	a = pad_key_action(0x38, PAD_MOD_COMMAND);
	CHECK(a.function == PAD_DO_MODIFIER && a.key == 0x38 && !a.modifiers);
	CHECK(!strcmp(pad_key_name(0x24), "Return") && !pad_key_name(0x7F));
	struct pad_map x = pad_default_map, y = pad_default_map;
	CHECK(pad_maps_equal(&x, &y));
	y.control[PAD_A].chr = 'x';                             /* (a key's character isn't what it does) */
	CHECK(pad_maps_equal(&x, &y));
	y.stick[1] = PAD_STICK_KEYS;
	CHECK(!pad_maps_equal(&x, &y));
}

void test_pad(void)
{
	test_pipp();
	test_default();
	test_applejack();
	test_two_applejacks();
	test_own_map();
	test_action_names();
	test_mouse();
	test_taken();
	test_forget();
}
