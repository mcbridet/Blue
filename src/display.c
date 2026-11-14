#include "display.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ui.h"

/* The parts of the SDL2 ABI we use (stable across SDL 2.x). */
#define SDL_INIT_VIDEO 0x00000020u
#define SDL_WINDOWPOS_CENTERED 0x2FFF0000
#define SDL_WINDOW_FULLSCREEN 0x00000001u
#define SDL_WINDOW_RESIZABLE 0x00000020u
#define SDL_WINDOW_MAXIMIZED 0x00000080u
#define SDL_WINDOW_ALLOW_HIGHDPI 0x00002000u
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001u
#define SDL_RENDERER_SOFTWARE 0x00000001u
#define SDL_RENDERER_TARGETTEXTURE 0x00000008u
#define SDL_PIXELFORMAT_RGB24 0x17101803u
#define SDL_PIXELFORMAT_ARGB8888 0x16362004u
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_TEXTUREACCESS_TARGET 2
#define SDL_BLENDMODE_NONE 0
#define SDL_HINT_OVERRIDE 2
#define SDL_HINT_RENDER_SCALE_QUALITY "SDL_RENDER_SCALE_QUALITY"   /* read when a texture is made */
#define SDL_HINT_WAYLAND_SCALE_TO_DISPLAY "SDL_VIDEO_WAYLAND_SCALE_TO_DISPLAY"
#define SDL_QUIT 0x100u
#define SDL_WINDOWEVENT 0x200u
#define SDL_WINDOWEVENT_EXPOSED 3u
#define SDL_WINDOWEVENT_RESIZED 5u      /* by the user or the window manager, not by us */
#define SDL_WINDOWEVENT_SIZE_CHANGED 6u /* by anyone */
#define SDL_WINDOWEVENT_FOCUS_LOST 13u
#define SDL_KEYDOWN 0x300u
#define SDL_KEYUP 0x301u
#define SDL_MOUSEMOTION 0x400u
#define SDL_MOUSEBUTTONDOWN 0x401u
#define SDL_MOUSEBUTTONUP 0x402u
#define SDL_MOUSEWHEEL 0x403u
#define SDL_MOUSEWHEEL_FLIPPED 1u
#define SDL_RENDER_TARGETS_RESET 0x2000u /* (render targets lost what was drawn in them) */
#define SDL_DROPFILE 0x1000u
#define SDL_DROPTEXT 0x1001u
#define SDL_SCANCODE_RETURN 40u
#define SDL_SCANCODE_ESCAPE 41u
#define SDL_SCANCODE_TAB 43u
#define SDL_SCANCODE_SPACE 44u
#define SDL_SCANCODE_CAPSLOCK 57u
#define SDL_SCANCODE_F12 69u
#define SDL_SCANCODE_RIGHT 79u
#define SDL_SCANCODE_LEFT 80u
#define SDL_SCANCODE_DOWN 81u
#define SDL_SCANCODE_UP 82u
#define SDL_SCANCODE_KP_ENTER 88u
#define SDL_SCANCODE_LCTRL 224u
#define SDL_SCANCODE_LALT 226u
#define SDL_SCANCODE_RCTRL 228u
#define SDL_SCANCODE_RALT 230u

typedef union {
	uint32_t type;
	struct {
		uint32_t type, timestamp, window;
		uint8_t state, repeat, pad2, pad3;
		int32_t scancode, sym;
		uint16_t mod;
		uint32_t unused;
	} key;
	struct {
		uint32_t type, timestamp, window, which, state;
		int32_t x, y, xrel, yrel;
	} motion;
	struct {
		uint32_t type, timestamp, window;
		uint8_t event, pad1, pad2, pad3;
		int32_t data1, data2;
	} window;
	struct {
		uint32_t type, timestamp, window, which;
		uint8_t button, state, clicks, pad;
		int32_t x, y;
	} button;
	struct {
		uint32_t type, timestamp, window, which;
		int32_t x, y;
		uint32_t direction;
	} wheel;
	struct {
		uint32_t type, timestamp;
		char *file;                     /* (SDL's to free) */
		uint32_t window;
	} drop;
	uint8_t padding[56];
} SDL_Event;

typedef struct {
	const char *name;
	uint32_t flags, num_texture_formats, texture_formats[16];
	int max_texture_width, max_texture_height;
} SDL_RendererInfo;

typedef struct {
	int x, y, w, h;
} SDL_Rect;

struct sdl {
	void *lib;
	int (*Init)(uint32_t);
	void (*Quit)(void);
	const char *(*GetError)(void);
	int (*SetHintWithPriority)(const char *, const char *, int);
	void *(*CreateWindow)(const char *, int, int, int, int, uint32_t);
	void (*DestroyWindow)(void *);
	void *(*CreateRenderer)(void *, int, uint32_t);
	void (*DestroyRenderer)(void *);
	int (*GetRendererInfo)(void *, SDL_RendererInfo *);
	int (*GetRendererOutputSize)(void *, int *, int *);
	void *(*CreateTexture)(void *, uint32_t, int, int, int);
	void (*DestroyTexture)(void *);
	int (*SetTextureBlendMode)(void *, int);
	int (*UpdateTexture)(void *, const void *, const void *, int);
	int (*SetRenderTarget)(void *, void *);
	int (*RenderClear)(void *);
	int (*RenderCopy)(void *, void *, const void *, const void *);
	void (*RenderPresent)(void *);
	int (*PollEvent)(SDL_Event *);
	int (*WaitEventTimeout)(void *, int);
	int (*ShowCursor)(int);
	int (*SetRelativeMouseMode)(int);
	void (*SetWindowTitle)(void *, const char *);
	int (*SetWindowFullscreen)(void *, uint32_t);
	void (*SetWindowSize)(void *, int, int);
	void (*GetWindowSize)(void *, int *, int *);
	uint32_t (*GetWindowFlags)(void *);
	void (*free)(void *);
};

struct display {
	struct sdl sdl;
	void *window, *renderer;
	void *texture;                  /* the screen, drawn nearest-neighbour */
	void *scaled;                   /* ... at a whole multiple of its size, drawn filtered */
	bool scaled_stale;              /* ... from an older screen */
	unsigned width, height;         /* the screen's size */
	unsigned scaled_by;             /* the multiple `scaled` holds */
	int max_texture;                /* longest texture side the renderer takes (0: any) */
	bool prescale;                  /* render targets, on a renderer fast enough for them */
	bool integer;                   /* whole multiples only */
	double density;                 /* window pixels per unit when it was last fitted */
	bool caps_lock;
	bool ctrl[2], alt[2];           /* (left and right) held, for the release key */
	bool release;                   /* the release key was pressed: an event to deliver */
	struct ui *ui;                  /* the menu (F12), if this SDL can draw it */
	bool menu;                      /* it's open: input goes to it */
	bool menu_event;                /* F12 or Esc: a DISPLAY_EVENT_MENU to deliver */
	enum ui_action menu_action;     /* chosen in it, to deliver (or carry out) */
	double menu_time;               /* (when it was last drawn, in seconds) */
	enum ui_mouse menu_mouse;       /* what it shows */
	char menu_status[480];
	bool prompt;                    /* a question or news is shown (display_prompt()): input goes to it */
	char prompt_text[1024], prompt_yes[32], prompt_no[32];
	bool fullscreen;
	bool status_bar;                /* shown, below the screen (only with the menu's SDL) */
	struct ui_status bar;           /* what it shows */
	bool bar_stale;                 /* ... has changed since it was drawn */
	bool resize;                    /* a DISPLAY_EVENT_RESIZE to deliver: it came or went, the window kept its size */
	bool captured;                  /* the pointer is captured (display_capture_mouse()) */
	bool has_floppy;                /* the menu shows the floppy drive: */
	struct ui_floppy floppy;
	char drop[1024];                /* the file last dropped on the window */
};

/* The status bar's height in the window's units (0: hidden) */
static int bar_units(const struct display *d)
{
	return d->status_bar ? UI_STATUS_BAR_HEIGHT : 0;
}

/* ... and in its pixels, in a window wh units and ph pixels high */
static int bar_pixels(const struct display *d, int wh, int ph)
{
	return wh > 0 ? (int)lround((double)bar_units(d) * ph / wh) : 0;
}

/* The menu's renderer: SDL 2.0.18's functions (if there are none, there's no menu) */
static struct ui *open_menu(struct display *d)
{
	struct ui_renderer r = { .renderer = d->renderer };
#define SYM(field, name) (*(void **)&r.field = dlsym(d->sdl.lib, name))
	SYM(SetTextureScaleMode, "SDL_SetTextureScaleMode");    /* (2.0.12; filtering by the hint otherwise) */
	if (!SYM(CreateTexture, "SDL_CreateTexture") || !SYM(DestroyTexture, "SDL_DestroyTexture") ||
	    !SYM(UpdateTexture, "SDL_UpdateTexture") || !SYM(SetTextureBlendMode, "SDL_SetTextureBlendMode") ||
	    !SYM(RenderSetScale, "SDL_RenderSetScale") || !SYM(RenderSetClipRect, "SDL_RenderSetClipRect") ||
	    !SYM(RenderGeometryRaw, "SDL_RenderGeometryRaw")) {
		fprintf(stderr, "display: SDL is older than 2.0.18: no menu (F12 just pauses)\n");
		return NULL;
	}
#undef SYM
	return ui_open(&r);
}

static bool load_sdl(struct sdl *s)
{
	/* (bare names aren't looked for in Homebrew's or MacPorts' directories on macOS) */
	static const char *const names[] = { "libSDL2-2.0.so.0", "libSDL2.so", "libSDL2-2.0.dylib",
					     "/opt/homebrew/lib/libSDL2-2.0.dylib", "/opt/local/lib/libSDL2-2.0.dylib",
					     "SDL2.framework/SDL2" };
	for (size_t k = 0; k < sizeof names / sizeof names[0] && !s->lib; k++)
		s->lib = dlopen(names[k], RTLD_NOW | RTLD_LOCAL);
	if (!s->lib)
		return false;

#define SYM(field, name) \
	do { \
		*(void **)&s->field = dlsym(s->lib, name); \
		if (!s->field) \
			return false; \
	} while (0)
	SYM(Init, "SDL_Init");
	SYM(Quit, "SDL_Quit");
	SYM(GetError, "SDL_GetError");
	SYM(SetHintWithPriority, "SDL_SetHintWithPriority");
	SYM(CreateWindow, "SDL_CreateWindow");
	SYM(DestroyWindow, "SDL_DestroyWindow");
	SYM(CreateRenderer, "SDL_CreateRenderer");
	SYM(DestroyRenderer, "SDL_DestroyRenderer");
	SYM(GetRendererInfo, "SDL_GetRendererInfo");
	SYM(GetRendererOutputSize, "SDL_GetRendererOutputSize");
	SYM(CreateTexture, "SDL_CreateTexture");
	SYM(DestroyTexture, "SDL_DestroyTexture");
	SYM(SetTextureBlendMode, "SDL_SetTextureBlendMode");
	SYM(UpdateTexture, "SDL_UpdateTexture");
	SYM(SetRenderTarget, "SDL_SetRenderTarget");
	SYM(RenderClear, "SDL_RenderClear");
	SYM(RenderCopy, "SDL_RenderCopy");
	SYM(RenderPresent, "SDL_RenderPresent");
	SYM(PollEvent, "SDL_PollEvent");
	SYM(WaitEventTimeout, "SDL_WaitEventTimeout");
	SYM(ShowCursor, "SDL_ShowCursor");
	SYM(SetRelativeMouseMode, "SDL_SetRelativeMouseMode");
	SYM(SetWindowTitle, "SDL_SetWindowTitle");
	SYM(SetWindowFullscreen, "SDL_SetWindowFullscreen");
	SYM(SetWindowSize, "SDL_SetWindowSize");
	SYM(GetWindowSize, "SDL_GetWindowSize");
	SYM(GetWindowFlags, "SDL_GetWindowFlags");
	SYM(free, "SDL_free");
#undef SYM
	return true;
}

struct display_view display_view(unsigned screen_w, unsigned screen_h, int out_w, int out_h, bool integer)
{
	struct display_view v = { .screen_w = screen_w, .screen_h = screen_h };
	if (!screen_w || !screen_h || out_w <= 0 || out_h <= 0)
		return v;
	unsigned n = (unsigned)out_w / screen_w < (unsigned)out_h / screen_h ? (unsigned)out_w / screen_w
									    : (unsigned)out_h / screen_h;
	int spare_w = out_w - (int)(n * screen_w), spare_h = out_h - (int)(n * screen_h);
	/* (a whole multiple is exact: worth leaving a pixel or two unfilled for) */
	if (n >= 1 && (integer || (spare_w < spare_h ? spare_w : spare_h) <= 2)) {
		v.w = (int)(n * screen_w);
		v.h = (int)(n * screen_h);
	} else {
		double s = fmin((double)out_w / screen_w, (double)out_h / screen_h);
		v.w = (int)lround(screen_w * s);
		v.h = (int)lround(screen_h * s);
		v.w = v.w < 1 ? 1 : v.w > out_w ? out_w : v.w;
		v.h = v.h < 1 ? 1 : v.h > out_h ? out_h : v.h;
		v.prescale = (unsigned)ceil(s);
	}
	v.x = (out_w - v.w) / 2;
	v.y = (out_h - v.h) / 2;
	return v;
}

void display_view_point(const struct display_view *v, double px, double py, int *x, int *y)
{
	*x = v->w > 0 ? (int)floor((px - v->x) * v->screen_w / v->w) : 0;
	*y = v->h > 0 ? (int)floor((py - v->y) * v->screen_h / v->h) : 0;
}

/* The window's size in its own units (those of mouse positions) and in pixels; returns pixels per unit. */
static double window_size(struct display *d, int *ww, int *wh, int *pw, int *ph)
{
	d->sdl.GetWindowSize(d->window, ww, wh);
	if (d->sdl.GetRendererOutputSize(d->renderer, pw, ph) != 0 || *pw <= 0 || *ph <= 0) {
		*pw = *ww;
		*ph = *wh;
	}
	return *ww > 0 && *pw > 0 ? (double)*pw / *ww : 1.0;
}

/* With integer scaling, the multiple a screen sized to the window is shown at: the display's scale, rounded */
static unsigned natural_multiple(double density)
{
	return density < 1.5 ? 1 : (unsigned)(density + 0.5);
}

/* The screen size a window of ww x wh units shows at its natural scale (above the status bar) */
static void screen_for_window(const struct display *d, int ww, int wh, double density, int *sw, int *sh)
{
	wh = wh > bar_units(d) ? wh - bar_units(d) : 0;
	if (!d->integer) {
		*sw = ww;
		*sh = wh;
		return;
	}
	int n = (int)natural_multiple(density);
	*sw = (int)lround(ww * density) / n;
	*sh = (int)lround(wh * density) / n;
}

/* ... and the window size that shows a screen at it */
static void window_for_screen(const struct display *d, unsigned sw, unsigned sh, double density, int *ww, int *wh)
{
	if (!d->integer) {
		*ww = (int)sw;
		*wh = (int)sh + bar_units(d);
		return;
	}
	unsigned n = natural_multiple(density);
	*ww = (int)ceil(sw * n / density - 0.01);
	*wh = (int)ceil(sh * n / density - 0.01) + bar_units(d);
}

/*
 * The window takes the screen's size, unless it is that size already (to
 * the 8 pixels screens sized to the window are rounded to): it may be being
 * resized.
 */
static void fit_window(struct display *d)
{
	int ww, wh, pw, ph, sw, sh;
	d->density = window_size(d, &ww, &wh, &pw, &ph);
	screen_for_window(d, ww, wh, d->density, &sw, &sh);
	if (sw >= (int)d->width && sw < (int)d->width + 8 && sh == (int)d->height)
		return;
	window_for_screen(d, d->width, d->height, d->density, &ww, &wh);
	d->sdl.SetWindowSize(d->window, ww, wh);
}

static void *make_texture(struct display *d, uint32_t format, int access, unsigned width, unsigned height,
			  bool filtered)
{
	d->sdl.SetHintWithPriority(SDL_HINT_RENDER_SCALE_QUALITY, filtered ? "linear" : "nearest", SDL_HINT_OVERRIDE);
	return d->sdl.CreateTexture(d->renderer, format, access, (int)width, (int)height);
}

static bool set_screen_size(struct display *d, unsigned width, unsigned height)
{
	if (d->texture)
		d->sdl.DestroyTexture(d->texture);
	if (d->scaled)
		d->sdl.DestroyTexture(d->scaled);
	d->scaled = NULL;
	d->scaled_by = 0;
	d->texture = make_texture(d, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, width, height, false);
	if (!d->texture)
		return false;
	d->width = width;
	d->height = height;
	return true;
}

/* The screen drawn at k times its size, to filter down from; NULL if it can't be. */
static void *prescaled(struct display *d, unsigned k)
{
	if (k != d->scaled_by) {
		if (d->scaled)
			d->sdl.DestroyTexture(d->scaled);
		d->scaled = make_texture(d, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, d->width * k,
					 d->height * k, true);
		d->scaled_by = k;
		if (!d->scaled) {
			d->prescale = false;
			return NULL;
		}
		d->sdl.SetTextureBlendMode(d->scaled, SDL_BLENDMODE_NONE);
		d->scaled_stale = true;
	}
	if (!d->scaled_stale)
		return d->scaled;
	if (d->sdl.SetRenderTarget(d->renderer, d->scaled) != 0)
		return NULL;
	d->sdl.RenderCopy(d->renderer, d->texture, NULL, NULL);
	d->sdl.SetRenderTarget(d->renderer, NULL);
	d->scaled_stale = false;
	return d->scaled;
}

/* Where the screen is drawn: in the window above the status bar */
static struct display_view screen_view(struct display *d, int *ww, int *wh, int *pw, int *ph)
{
	window_size(d, ww, wh, pw, ph);
	return display_view(d->width, d->height, *pw, *ph - bar_pixels(d, *wh, *ph), d->integer);
}

static void draw(struct display *d)
{
	int ww, wh, pw, ph;
	struct display_view v = screen_view(d, &ww, &wh, &pw, &ph);
	unsigned k = v.prescale, max = (unsigned)d->max_texture;
	while (k > 1 && max && (d->width * k > max || d->height * k > max))
		k--;
	void *src = k && d->prescale ? prescaled(d, k) : NULL;
	SDL_Rect dst = { v.x, v.y, v.w, v.h };
	d->sdl.RenderClear(d->renderer);
	d->sdl.RenderCopy(d->renderer, src ? src : d->texture, NULL, &dst);
	if (d->ui && (d->menu || d->status_bar || d->prompt)) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		double now = (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
		struct ui_state s = {
			.menu = d->menu,
			/* (whole pixels: the screen is drawn above them) */
			.status_bar = ph > 0 ? (float)bar_pixels(d, wh, ph) * (float)wh / (float)ph : 0,
			.bar = d->bar,
			.fullscreen = d->fullscreen,
			.mouse = d->menu_mouse,
			.news = d->menu_status[0] ? d->menu_status : NULL,
			.prompt = d->prompt ? d->prompt_text : NULL,
			.prompt_yes = d->prompt_yes,
			.prompt_no = d->prompt_no[0] ? d->prompt_no : NULL,
			.floppy = d->has_floppy ? &d->floppy : NULL,
		};
		enum ui_action action = ui_draw(d->ui, &s, ww, wh, ww > 0 ? (float)pw / (float)ww : 1, now - d->menu_time);
		if (action != UI_NONE)
			d->menu_action = action;
		d->menu_time = now;
	}
	d->bar_stale = false;
	d->sdl.RenderPresent(d->renderer);
}

struct display *display_open(const char *title, unsigned width, unsigned height, bool integer)
{
	struct display *d = calloc(1, sizeof *d);
	if (!d)
		return NULL;
	if (!load_sdl(&d->sdl)) {
		fprintf(stderr, "display: SDL2 is not available; running without a window\n");
		goto fail;
	}
	/* sdl2-compat (SDL2 on SDL3) makes a window's units pixels on Wayland, unless told
	 * the program handles high-density displays itself, as this one does */
	if (!getenv(SDL_HINT_WAYLAND_SCALE_TO_DISPLAY))
		d->sdl.SetHintWithPriority(SDL_HINT_WAYLAND_SCALE_TO_DISPLAY, "0", SDL_HINT_OVERRIDE);
	if (d->sdl.Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "display: %s\n", d->sdl.GetError());
		goto fail;
	}
	d->integer = integer;
	/* (on high-density displays, drawn at their full resolution rather than scaled up by the system) */
	d->window = d->sdl.CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
					(int)width, (int)height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	if (d->window)
		d->renderer = d->sdl.CreateRenderer(d->window, -1, 0);
	if (!d->renderer || !set_screen_size(d, width, height)) {
		fprintf(stderr, "display: %s\n", d->sdl.GetError());
		display_close(d);
		return NULL;
	}
	SDL_RendererInfo info;
	if (d->sdl.GetRendererInfo(d->renderer, &info) == 0) {
		/* (in software, drawing the screen at several times its size each frame costs too much) */
		d->prescale = (info.flags & SDL_RENDERER_TARGETTEXTURE) && !(info.flags & SDL_RENDERER_SOFTWARE);
		d->max_texture = info.max_texture_width < info.max_texture_height ? info.max_texture_width
										  : info.max_texture_height;
	}
	fit_window(d);
	d->sdl.ShowCursor(0);           /* Mac OS draws its own */
	d->ui = open_menu(d);
	return d;
fail:
	if (d->sdl.lib)
		dlclose(d->sdl.lib);
	free(d);
	return NULL;
}

void display_close(struct display *d)
{
	if (!d)
		return;
	ui_close(d->ui);
	if (d->scaled)
		d->sdl.DestroyTexture(d->scaled);
	if (d->texture)
		d->sdl.DestroyTexture(d->texture);
	if (d->renderer)
		d->sdl.DestroyRenderer(d->renderer);
	if (d->window)
		d->sdl.DestroyWindow(d->window);
	d->sdl.Quit();
	dlclose(d->sdl.lib);
	free(d);
}

void display_present(struct display *d, const uint8_t *rgb, unsigned width, unsigned height)
{
	if (width != d->width || height != d->height) {
		if (!set_screen_size(d, width, height))
			return;
		fit_window(d);
	} else if (d->integer) {
		/* the window's natural size depends on the display's scale: known once the window
		 * is shown, and changed by moving it to another display */
		int ww, wh, pw, ph;
		if (fabs(window_size(d, &ww, &wh, &pw, &ph) - d->density) > 0.02)
			fit_window(d);
	}
	d->sdl.UpdateTexture(d->texture, NULL, rgb, (int)width * 3);
	d->scaled_stale = true;
	draw(d);
}

bool display_wait(struct display *d, unsigned ms)
{
	return d->sdl.WaitEventTimeout(NULL, (int)ms) == 1;
}

void display_capture_mouse(struct display *d, bool on)
{
	d->captured = on;
	d->sdl.SetRelativeMouseMode(on);
	d->sdl.SetWindowTitle(d->window, on ? "syshm (Control-Option releases the mouse)" : "syshm");
}

/* A position in the window, in its units, to a screen pixel */
static void to_screen(struct display *d, int x, int y, int *sx, int *sy)
{
	int ww, wh, pw, ph;
	struct display_view v = screen_view(d, &ww, &wh, &pw, &ph);
	/* (positions are whole units, a unit maybe more than a pixel: take its middle) */
	display_view_point(&v, (x + 0.5) * pw / (ww > 0 ? ww : 1), (y + 0.5) * ph / (wh > 0 ? wh : 1), sx, sy);
}

/* Whether a position in the window, in its units, is on the status bar */
static bool on_status_bar(struct display *d, int y)
{
	int ww, wh;
	d->sdl.GetWindowSize(d->window, &ww, &wh);
	return d->status_bar && y >= wh - bar_units(d);
}

/*
 * Shows or hides the status bar. The window grows or shrinks to keep the
 * screen's size, unless it keeps its size (fullscreen, maximized): then
 * it's the screen that fits it that changes, as if the user had resized it.
 */
static void show_status_bar(struct display *d, bool on)
{
	if (on == d->status_bar || (on && !d->ui))
		return;
	int ww, wh;
	d->sdl.GetWindowSize(d->window, &ww, &wh);
	d->status_bar = on;
	if (d->sdl.GetWindowFlags(d->window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED))
		d->resize = true;
	else
		d->sdl.SetWindowSize(d->window, ww, wh + (on ? UI_STATUS_BAR_HEIGHT : -UI_STATUS_BAR_HEIGHT));
	draw(d);
}

static void menu_key(struct display *d, unsigned scancode, bool down);

bool display_poll(struct display *d, struct display_event *ev)
{
	SDL_Event e;
	memset(ev, 0, sizeof *ev);
	if (d->release) {
		d->release = false;
		ev->type = DISPLAY_EVENT_RELEASE_MOUSE;
		return true;
	}
	if (d->menu_event) {
		d->menu_event = false;
		ev->type = DISPLAY_EVENT_MENU;
		return true;
	}
	if (d->menu_action != UI_NONE) {
		enum ui_action action = d->menu_action;
		d->menu_action = UI_NONE;
		if (action == UI_FULLSCREEN) {
			d->fullscreen = !d->fullscreen;
			d->sdl.SetWindowFullscreen(d->window, d->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
		} else if (action == UI_STATUS_BAR) {
			show_status_bar(d, !d->status_bar);
		} else {
			if (action == UI_PROMPT_YES || action == UI_PROMPT_NO)
				display_prompt(d, NULL, NULL, NULL);
			ev->type = action == UI_RESUME ? DISPLAY_EVENT_MENU : DISPLAY_EVENT_MENU_ACTION;
			ev->action = action;
			return true;
		}
	}
	if (d->resize) {
		d->resize = false;
		int ww, wh, pw, ph;
		double density = window_size(d, &ww, &wh, &pw, &ph);
		ev->type = DISPLAY_EVENT_RESIZE;
		screen_for_window(d, ww, wh, density, &ev->x, &ev->y);
		return true;
	}
	while (d->sdl.PollEvent(&e)) {
		switch (e.type) {
		case SDL_QUIT:
			ev->type = DISPLAY_EVENT_QUIT;
			return true;
		case SDL_RENDER_TARGETS_RESET:
			d->scaled_stale = true;
			ev->type = DISPLAY_EVENT_EXPOSE;
			return true;
		case SDL_WINDOWEVENT: {
			if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
				/* (keys let go elsewhere aren't seen) */
				d->ctrl[0] = d->ctrl[1] = d->alt[0] = d->alt[1] = false;
				continue;
			}
			/* (some compositors, Wayland's, resize the window only as fast as it's drawn) */
			if (e.window.event == SDL_WINDOWEVENT_EXPOSED || e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
				ev->type = DISPLAY_EVENT_EXPOSE;
				return true;
			}
			if (e.window.event != SDL_WINDOWEVENT_RESIZED)
				continue;
			int ww, wh, pw, ph;
			double density = window_size(d, &ww, &wh, &pw, &ph);
			ev->type = DISPLAY_EVENT_RESIZE;
			screen_for_window(d, e.window.data1, e.window.data2, density, &ev->x, &ev->y);
			return true;
		}
		case SDL_KEYDOWN:
		case SDL_KEYUP: {
			bool down = e.type == SDL_KEYDOWN;
			if (e.key.repeat)
				continue;
			unsigned sc = (unsigned)e.key.scancode;
			/* F12 opens and closes the menu (Esc closes it too): the guest doesn't get them */
			if (sc == SDL_SCANCODE_F12 || (d->menu && sc == SDL_SCANCODE_ESCAPE)) {
				if (!down)
					continue;
				ev->type = DISPLAY_EVENT_MENU;
				return true;
			}
			if (d->menu) {
				menu_key(d, sc, down);
				continue;
			}
			if (d->prompt) {                /* (Esc is its "no") */
				if (sc != SDL_SCANCODE_ESCAPE) {
					menu_key(d, sc, down);
					continue;
				}
				if (!down)
					continue;
				ev->type = DISPLAY_EVENT_MENU_ACTION;
				ev->action = d->prompt_no[0] ? UI_PROMPT_NO : UI_PROMPT_YES;
				display_prompt(d, NULL, NULL, NULL);
				return true;
			}
			/* Control and Option (Ctrl and Alt) together: the release key (and keys to the guest too) */
			if (sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL)
				d->ctrl[sc == SDL_SCANCODE_RCTRL] = down;
			if (sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT)
				d->alt[sc == SDL_SCANCODE_RALT] = down;
			if (down && (d->ctrl[0] || d->ctrl[1]) && (d->alt[0] || d->alt[1]) &&
			    (sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL || sc == SDL_SCANCODE_LALT ||
			     sc == SDL_SCANCODE_RALT))
				d->release = true;
			if (sc == SDL_SCANCODE_CAPSLOCK) {
				/* the ADB caps lock key latches: down when engaged, up when released */
				if (!down)
					continue;
				d->caps_lock = !d->caps_lock;
				down = d->caps_lock;
			}
			uint8_t code = display_hid_to_adb((unsigned)e.key.scancode);
			if (code == 0xFF)
				continue;
			ev->type = DISPLAY_EVENT_KEY;
			ev->adb_key = code;
			ev->down = down;
			return true;
		}
		case SDL_MOUSEMOTION:
			if (d->menu || d->prompt) {
				if (d->ui)
					ui_mouse_move(d->ui, (float)e.motion.x, (float)e.motion.y);
				continue;
			}
			ev->type = DISPLAY_EVENT_MOUSE_MOVE;
			to_screen(d, e.motion.x, e.motion.y, &ev->x, &ev->y);
			ev->dx = e.motion.xrel;
			ev->dy = e.motion.yrel;
			return true;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			if (d->menu || d->prompt) {
				/* (SDL's buttons are left, middle, right; Dear ImGui's left, right, middle) */
				static const int buttons[4] = { -1, 0, 2, 1 };
				if (d->ui && e.button.button < 4)
					ui_mouse_button(d->ui, buttons[e.button.button], e.type == SDL_MOUSEBUTTONDOWN);
				continue;
			}
			/* (a click on the status bar isn't one at the bottom of the screen; letting go anywhere is) */
			if (e.type == SDL_MOUSEBUTTONDOWN && !d->captured && on_status_bar(d, e.button.y))
				continue;
			ev->type = DISPLAY_EVENT_MOUSE_BUTTON;
			ev->button = e.button.button - 1;
			ev->down = e.type == SDL_MOUSEBUTTONDOWN;
			to_screen(d, e.button.x, e.button.y, &ev->x, &ev->y);
			return true;
		case SDL_MOUSEWHEEL:
			if ((d->menu || d->prompt) && d->ui) {
				float flip = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
				ui_mouse_wheel(d->ui, flip * (float)e.wheel.x, flip * (float)e.wheel.y);
			}
			continue;
		case SDL_DROPFILE:
		case SDL_DROPTEXT:
			if (!e.drop.file)
				continue;
			snprintf(d->drop, sizeof d->drop, "%s", e.drop.file);
			d->sdl.free(e.drop.file);
			if (e.type == SDL_DROPTEXT)
				continue;
			ev->type = DISPLAY_EVENT_DROP_FILE;
			ev->path = d->drop;
			return true;
		default:
			continue;
		}
	}
	return false;
}

/* The keys the menu takes (for moving about it from the keyboard) */
static void menu_key(struct display *d, unsigned scancode, bool down)
{
	static const struct { unsigned scancode; enum ui_key key; } keys[] = {
		{ SDL_SCANCODE_TAB, UI_KEY_TAB }, { SDL_SCANCODE_LEFT, UI_KEY_LEFT }, { SDL_SCANCODE_RIGHT, UI_KEY_RIGHT },
		{ SDL_SCANCODE_UP, UI_KEY_UP }, { SDL_SCANCODE_DOWN, UI_KEY_DOWN }, { SDL_SCANCODE_SPACE, UI_KEY_SPACE },
		{ SDL_SCANCODE_RETURN, UI_KEY_ENTER }, { SDL_SCANCODE_KP_ENTER, UI_KEY_ENTER },
	};
	for (size_t k = 0; d->ui && k < sizeof keys / sizeof keys[0]; k++)
		if (keys[k].scancode == scancode)
			ui_key(d->ui, keys[k].key, down);
}

void display_menu(struct display *d, bool open)
{
	d->menu = open;
	d->menu_event = false;
	d->menu_action = UI_NONE;
	d->ctrl[0] = d->ctrl[1] = d->alt[0] = d->alt[1] = false;
	/* (the guest's cursor doesn't move meanwhile: the host's is shown for the menu) */
	d->sdl.ShowCursor((open || d->prompt) && d->ui ? 1 : 0);
	if (!d->ui)
		d->sdl.SetWindowTitle(d->window, open ? "syshm (paused: F12 resumes)" : "syshm");
	if (open) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		d->menu_time = (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
	}
	draw(d);
}

void display_menu_state(struct display *d, enum ui_mouse mouse, const char *status)
{
	d->menu_mouse = mouse;
	snprintf(d->menu_status, sizeof d->menu_status, "%s", status ? status : "");
}

void display_floppy_state(struct display *d, const struct ui_floppy *f)
{
	d->has_floppy = f != NULL;
	if (f)
		d->floppy = *f;
}

bool display_prompt(struct display *d, const char *text, const char *yes, const char *no)
{
	if (!d || (text && !d->ui))
		return false;
	d->prompt = text != NULL;
	snprintf(d->prompt_text, sizeof d->prompt_text, "%s", text ? text : "");
	snprintf(d->prompt_yes, sizeof d->prompt_yes, "%s", yes ? yes : "");
	snprintf(d->prompt_no, sizeof d->prompt_no, "%s", no ? no : "");
	if (d->menu_action == UI_PROMPT_YES || d->menu_action == UI_PROMPT_NO)
		d->menu_action = UI_NONE;
	d->ctrl[0] = d->ctrl[1] = d->alt[0] = d->alt[1] = false;
	d->sdl.ShowCursor((d->prompt || d->menu) && d->ui ? 1 : 0);
	draw(d);
	return true;
}

void display_refresh(struct display *d)
{
	draw(d);
}

void display_status_bar(struct display *d, bool on)
{
	show_status_bar(d, on);
}

void display_status(struct display *d, const struct ui_status *s)
{
	/* (copied whole, padding too, so that comparing them whole works: the caller clears it) */
	if (memcmp(&d->bar, s, sizeof d->bar) == 0)
		return;
	memcpy(&d->bar, s, sizeof d->bar);
	d->bar_stale = d->status_bar;
}

void display_update(struct display *d)
{
	if (d->bar_stale || d->prompt)  /* (a prompt answers the pointer, every frame) */
		draw(d);
}

uint8_t display_hid_to_adb(unsigned usage)
{
	/* USB HID keyboard usages 4..103 */
	static const uint8_t main_keys[100] = {
		[4] = 0x00, [5] = 0x0B, [6] = 0x08, [7] = 0x02, [8] = 0x0E, [9] = 0x03,         /* a-f */
		[10] = 0x05, [11] = 0x04, [12] = 0x22, [13] = 0x26, [14] = 0x28, [15] = 0x25,   /* g-l */
		[16] = 0x2E, [17] = 0x2D, [18] = 0x1F, [19] = 0x23, [20] = 0x0C, [21] = 0x0F,   /* m-r */
		[22] = 0x01, [23] = 0x11, [24] = 0x20, [25] = 0x09, [26] = 0x0D, [27] = 0x07,   /* s-x */
		[28] = 0x10, [29] = 0x06,                                                       /* y z */
		[30] = 0x12, [31] = 0x13, [32] = 0x14, [33] = 0x15, [34] = 0x17,                /* 1-5 */
		[35] = 0x16, [36] = 0x1A, [37] = 0x1C, [38] = 0x19, [39] = 0x1D,                /* 6-0 */
		[40] = 0x24, [41] = 0x35, [42] = 0x33, [43] = 0x30, [44] = 0x31,  /* return esc bksp tab space */
		[45] = 0x1B, [46] = 0x18, [47] = 0x21, [48] = 0x1E, [49] = 0x2A,  /* - = [ ] \ */
		[50] = 0x2A, [51] = 0x29, [52] = 0x27, [53] = 0x32, [54] = 0x2B,  /* # ; ' ` , */
		[55] = 0x2F, [56] = 0x2C, [57] = 0x39,                            /* . / caps */
		[58] = 0x7A, [59] = 0x78, [60] = 0x63, [61] = 0x76, [62] = 0x60,  /* F1-F5 */
		[63] = 0x61, [64] = 0x62, [65] = 0x64, [66] = 0x65, [67] = 0x6D,  /* F6-F10 */
		[68] = 0x67, [69] = 0x6F, [70] = 0x69, [71] = 0x6B, [72] = 0x71,  /* F11 F12 F13-F15 */
		[73] = 0x72, [74] = 0x73, [75] = 0x74, [76] = 0x75, [77] = 0x77,  /* help home pgup del end */
		[78] = 0x79, [79] = 0x3C, [80] = 0x3B, [81] = 0x3D, [82] = 0x3E,  /* pgdn right left down up */
		[83] = 0x47, [84] = 0x4B, [85] = 0x43, [86] = 0x4E, [87] = 0x45,  /* clear / * - + */
		[88] = 0x4C, [89] = 0x53, [90] = 0x54, [91] = 0x55, [92] = 0x56,  /* enter 1-4 */
		[93] = 0x57, [94] = 0x58, [95] = 0x59, [96] = 0x5B, [97] = 0x5C,  /* 5-9 */
		[98] = 0x52, [99] = 0x41,                                         /* 0 . */
	};
	if (usage >= 4 && usage < 100)
		return main_keys[usage];
	switch (usage) {
	case 100: return 0x0A;          /* ISO key next to left shift */
	case 103: return 0x51;          /* keypad = */
	case 224: case 228: return 0x36;        /* control */
	case 225: case 229: return 0x38;        /* shift */
	case 226: case 230: return 0x3A;        /* alt -> option */
	case 227: case 231: return 0x37;        /* GUI (the Mac command key) -> command */
	default: return 0xFF;
	}
}
