#include "display.h"

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/event.h>
#endif

#include "chooser.h"
#include "density.h"
#include "hostlib.h"
#include "icon.h"
#include "menubar.h"
#include "ui.h"

/* The parts of the SDL2 ABI we use (stable across SDL 2.x). */
#define SDL_INIT_VIDEO 0x00000020u
#define SDL_WINDOWPOS_CENTERED 0x2FFF0000
#define SDL_WINDOWPOS_CENTERED_DISPLAY(k) (0x2FFF0000 | (k))
#define SDL_WINDOW_FULLSCREEN 0x00000001u
#define SDL_WINDOW_HIDDEN 0x00000008u
#define SDL_WINDOW_RESIZABLE 0x00000020u
#define SDL_WINDOW_MAXIMIZED 0x00000080u
#define SDL_WINDOW_ALLOW_HIGHDPI 0x00002000u
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001u
#define SDL_RENDERER_SOFTWARE 0x00000001u
#define SDL_RENDERER_TARGETTEXTURE 0x00000008u
#define SDL_PIXELFORMAT_ARGB8888 0x16362004u
#define SDL_TEXTUREACCESS_STATIC 0
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_TEXTUREACCESS_TARGET 2
#define SDL_BLENDMODE_NONE 0
#define SDL_BLENDMODE_BLEND 1
#define SDL_BLENDFACTOR_ZERO 1
#define SDL_BLENDFACTOR_ONE 2
#define SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR 4
#define SDL_BLENDFACTOR_ONE_MINUS_DST_COLOR 8
#define SDL_BLENDOPERATION_ADD 1
#define SDL_HINT_NORMAL 1
#define SDL_HINT_OVERRIDE 2
#define SDL_HINT_RENDER_SCALE_QUALITY "SDL_RENDER_SCALE_QUALITY"   /* read when a texture is made */
#define SDL_HINT_WAYLAND_SCALE_TO_DISPLAY "SDL_VIDEO_WAYLAND_SCALE_TO_DISPLAY"
#define SDL_QUIT 0x100u
#define SDL_WINDOWEVENT 0x200u
#define SDL_WINDOWEVENT_SHOWN 1u
#define SDL_WINDOWEVENT_EXPOSED 3u
#define SDL_WINDOWEVENT_MOVED 4u
#define SDL_WINDOWEVENT_RESIZED 5u      /* by the user or the window manager, not by us */
#define SDL_WINDOWEVENT_SIZE_CHANGED 6u /* by anyone */
#define SDL_WINDOWEVENT_FOCUS_GAINED 12u
#define SDL_WINDOWEVENT_FOCUS_LOST 13u
#define SDL_WINDOWEVENT_DISPLAY_CHANGED 18u     /* (2.0.18) */
#define SDL_KEYDOWN 0x300u
#define SDL_KEYUP 0x301u
#define SDL_MOUSEMOTION 0x400u
#define SDL_MOUSEBUTTONDOWN 0x401u
#define SDL_MOUSEBUTTONUP 0x402u
#define SDL_MOUSEWHEEL 0x403u
#define SDL_MOUSEWHEEL_FLIPPED 1u
#define SDL_CLIPBOARDUPDATE 0x900u
#define SDL_RENDER_TARGETS_RESET 0x2000u /* (render targets lost what was drawn in them) */
#define SDL_DROPFILE 0x1000u
#define SDL_DROPTEXT 0x1001u
#define SDL_USEREVENT 0x8000u           /* (ours: a wake-up from the machine's thread) */
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

typedef struct {
	uint8_t major, minor, patch;
} SDL_version;

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
	int (*ComposeCustomBlendMode)(int, int, int, int, int, int);    /* (2.0.6; NULL before) */
	int (*SetRenderTarget)(void *, void *);
	int (*RenderClear)(void *);
	int (*RenderCopy)(void *, void *, const void *, const void *);
	void (*RenderPresent)(void *);
	int (*PollEvent)(SDL_Event *);
	int (*WaitEventTimeout)(SDL_Event *, int);
	int (*PushEvent)(SDL_Event *);
	int (*ShowCursor)(int);
	int (*SetRelativeMouseMode)(int);
	void (*SetWindowTitle)(void *, const char *);
	int (*SetWindowFullscreen)(void *, uint32_t);
	void (*SetWindowSize)(void *, int, int);
	void (*GetWindowSize)(void *, int *, int *);
	void (*SetWindowPosition)(void *, int, int);
	void (*GetWindowPosition)(void *, int *, int *);
	void (*ShowWindow)(void *);
	uint32_t (*GetWindowFlags)(void *);
	int (*GetWindowDisplayIndex)(void *);
	int (*GetDisplayBounds)(int, SDL_Rect *);
	void (*GetVersion)(SDL_version *);
	const char *(*GetCurrentVideoDriver)(void);
	int (*GetDisplayUsableBounds)(int, SDL_Rect *);         /* (these may be missing: before 2.0.5) */
	int (*GetWindowBordersSize)(void *, int *, int *, int *, int *);
	int (*GetDisplayDPI)(int, float *, float *, float *);
	char *(*GetClipboardText)(void);        /* (these may be missing) */
	int (*SetClipboardText)(const char *);
	void (*free)(void *);
	void *(*CreateRGBSurfaceFrom)(void *, int, int, int, int, uint32_t, uint32_t, uint32_t, uint32_t);
	void (*SetWindowIcon)(void *, void *);
	void (*FreeSurface)(void *);
};

#define QUEUE 1024                      /* events waiting for the machine, at most */
#define OUT 64                          /* ... and gathered meanwhile on the window's thread */
#define BANDS 8                         /* bands of rows uploaded at once ... */
#define BAND_GAP 32u                    /* ... rows fewer than this apart being one */

/* An event for the machine, with what it carries */
struct queued {
	struct display_event ev;
	char *path;                     /* (a dropped file's, to free) */
	unsigned prompt;                /* (a prompt's answer: which prompt it answers) */
};

/*
 * What the machine has handed the window and not yet taken (under
 * host.lock), each with whether it has changed since
 */
struct handed {
	bool cursor_changed;
	struct display_cursor cursor;
	uint32_t cursor_pixels[DISPLAY_CURSOR_MAX * DISPLAY_CURSOR_MAX];
	bool bar_changed;
	struct ui_status bar;
	bool menu_changed;              /* (opened or closed) */
	bool menu;
	bool info_changed;              /* (what it shows) */
	enum ui_mouse mouse;
	char status[480];
	bool has_drives;
	struct ui_drives drives;
	bool tools;
	char tools_why_not[160];
	bool prompt_changed;
	bool prompt;
	unsigned prompt_id;             /* (counts the prompts shown) */
	char prompt_text[2048], prompt_yes[32], prompt_no[32], prompt_alt[32];
	int capture;                    /* capture the pointer (1) or let it go (0); -1: as it is */
	int status_bar;                 /* show the status bar (1) or hide it (0); -1: as it is */
	int follows;                    /* the screen follows the window's size (1) or not (0); -1: as it was */
	int shortcuts;                  /* Command-Q and -H the menu bar's (1) or Mac OS's (0); -1: as they are */
	char *clipboard;                /* text for the host's clipboard (to free), or NULL */
	bool clipboard_asked;           /* the host's clipboard wanted (display_clipboard()) ... */
	bool clipboard_answered;        /* ... and here it is: */
	char *clipboard_text;
	bool close;                     /* the window is to go (display_close()) */
};

struct display {
	struct sdl sdl;
	bool integer;                   /* whole multiples only */
	struct ui *ui;                  /* the menu (F12), if this SDL can draw it */

	/* the window's thread's */
	void *window, *renderer;
	void *texture;                  /* the screen, drawn nearest-neighbour */
	void *scaled;                   /* ... at a whole multiple of its size, drawn filtered */
	unsigned scaled_from, scaled_to;        /* ... rows of it from an older screen */
	unsigned width, height;         /* the screen's size */
	unsigned scaled_by;             /* the multiple `scaled` holds */
	uint32_t *argb;                 /* the screen converted, what the texture is updated from */
	size_t argb_size;               /* (pixels it holds) */
	/* the pointer (Mac OS's hardware cursor), drawn over the screen */
	struct display_cursor cursor;
	uint32_t cursor_pixels[DISPLAY_CURSOR_MAX * DISPLAY_CURSOR_MAX];
	unsigned long cursor_images;    /* ... counts the images copied to cursor_pixels */
	void *cursor_color, *cursor_invert;     /* ... its pixels, and those that invert the screen */
	unsigned long cursor_image;     /* ... the image they hold (of cursor_images) */
	unsigned cursor_scale;          /* ... at this multiple of its size (0: none yet) */
	bool cursor_filtered;
	int invert_mode;                /* the blend mode that inverts (-1: none) */
	int max_texture;                /* longest texture side the renderer takes (0: any) */
	bool prescale;                  /* render targets, on a renderer fast enough for them */
	double density;                 /* window pixels per unit, as last looked at (check_display()) */
	unsigned scale_chosen;          /* the scale chosen: display pixels to a screen pixel (0: Auto) */
	unsigned scale;                 /* ... the one in use */
	unsigned scale_auto;            /* ... Auto's, on the display the window is on: */
	int display;                    /* ... which (-1: not known yet) */
	SDL_Rect display_bounds;        /* ... where it is, in units */
	double ppi;                     /* ... its pixels per inch (0: not known) */
	bool recheck;                   /* the window may have gone to another display, or its density changed */
	uint64_t checked_ns;            /* ... last looked at */
	bool compat;                    /* SDL 2 is sdl2-compat (on SDL 3), whose displays have no physical size */
	bool follows;                   /* the screen follows the window's size (display_follow()) */
	bool caps_lock;
	bool ctrl[2], alt[2];           /* (left and right) held, for the release key */
	bool menu;                      /* the menu's open: input goes to it */
	double menu_time;               /* (when it was last drawn, in seconds) */
	enum ui_mouse menu_mouse;       /* what it shows */
	char menu_status[480];
	bool has_drives;                /* ... drives with removable media: */
	struct ui_drives drives;
	bool prompt;                    /* a question or news is shown (display_prompt()): input goes to it */
	unsigned prompt_id;
	char prompt_text[2048], prompt_yes[32], prompt_no[32], prompt_alt[32];
	bool tools;                     /* the menu has Install Guest Tools... (display_tools_state()), */
	char tools_why_not[160];        /* ... enabled unless this says why not */
	bool fullscreen;
	bool status_bar;                /* shown, below the screen (only with the menu's SDL) */
	struct ui_status bar;           /* what it shows */
	bool captured;                  /* the pointer is captured (display_capture_mouse()) */
	bool stale;                     /* the window is to be drawn again */
	enum ui_action choice;          /* chosen in the menu or a prompt as it was drawn */
	struct chooser *chooser;        /* the host's file chooser, open for a drive's Insert... */
	unsigned chooser_drive;         /* ... the menu's drive k */
	struct queued out[OUT];         /* events for the machine, not yet queued */
	unsigned nout;
	struct display_times times;     /* (BLUE_WINDOW_PROFILE) */

	/* shared with the machine's thread, under host.lock */
	atomic_bool woken;              /* a wake-up is on its way to the window's thread */
	bool waiting;                   /* the machine's waits for input (see input_wait()) ... */
	int kq;                         /* ... in this kqueue, on macOS */
	struct queued queue[QUEUE];     /* events for the machine */
	unsigned head, count;
	struct display_frame frame;     /* the screen (see display_frame_begin()) */
	unsigned frame_w, frame_h, frame_bpp;
	size_t frame_rb, frame_bytes, frame_rows;      /* (and what's allocated) */
	bool frame_ended;               /* ... it has changed since the window took it */
	uint64_t frame_end_ns;          /* ... when it was last handed over */
	struct handed in;
	struct display_times times_out; /* (the window's thread's, as of its last turn) */
	bool times_read;                /* ... read since: its latency_max_ns starts again */

	/* the machine's */
	char drop[1024];                /* the file last dropped on the window */
};

/*
 * The thread running the window (the main one, in display_main()) and the
 * machine's: what they wait for, under its lock
 */
static struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;            /* signalled when anything below or in the display's shared part changes */
	bool running;                   /* display_main() runs the window's thread */
	bool done;                      /* the machine's thread is over */
	bool opening;                   /* display_open() asks for a window: */
	const char *title;
	unsigned width, height;
	bool integer;
	unsigned scale;
	bool opened;                    /* ... answered: */
	struct display *d;              /* the window, or NULL */
	bool closed;                    /* display_close() was answered */
} host = { .lock = PTHREAD_MUTEX_INITIALIZER };

static char failure[256] = "not known";         /* why display_open() opened no window */

const char *display_failure(void)
{
	return failure;
}

/*
 * SIGINT and SIGTERM end a window's run at once, as they always have: SDL
 * would make them SDL_QUIT, which now asks the user first, so the window
 * has handlers of its own while it's open (put in place before SDL's would
 * be: SDL leaves those it finds alone, as it does SIG_IGN, a background
 * job's), and passes them on as DISPLAY_EVENT_QUIT.
 */
static volatile sig_atomic_t signalled;
static const int caught_signals[2] = { SIGINT, SIGTERM };
static bool caught[2];

static void on_signal(int sig)
{
	(void)sig;
	signalled = 1;
}

static void catch_signals(bool on)
{
	for (int k = 0; k < 2; k++) {
		struct sigaction old, sa;
		memset(&sa, 0, sizeof sa);
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_RESTART;
		if (on && sigaction(caught_signals[k], NULL, &old) == 0 && !(old.sa_flags & SA_SIGINFO) &&
		    old.sa_handler == SIG_DFL) {
			sa.sa_handler = on_signal;
			caught[k] = sigaction(caught_signals[k], &sa, NULL) == 0;
		} else if (!on && caught[k]) {
			sa.sa_handler = SIG_DFL;
			sigaction(caught_signals[k], &sa, NULL);
			caught[k] = false;
		}
	}
}

static uint64_t clock_ns(clockid_t clock)
{
	struct timespec ts;
	clock_gettime(clock, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* A part's time, from `from` and `cpu_from` (clock_ns()'s), to now; returns the clock now */
static uint64_t timed(struct display *d, unsigned part, uint64_t from, uint64_t *cpu_from)
{
	uint64_t now = clock_ns(CLOCK_MONOTONIC), cpu = clock_ns(CLOCK_THREAD_CPUTIME_ID);
	d->times.ns[part] += now - from;
	d->times.cpu_ns[part] += cpu - *cpu_from;
	*cpu_from = cpu;
	return now;
}

/*
 * The machine's thread waits for input (display_wait()) at most `ns`
 * nanoseconds; then the window's thread, queueing some, signals it
 * (signal_input()). It waits to time the guest's naps, so on time: on
 * macOS a sleep or a condition variable's wait for a thread not in front
 * came 15-25% late (16 ms became 19), and a kqueue timer that may not be
 * put off comes within 0.05 ms, so it waits in a kqueue there (for the
 * timer, or the signal).
 */
static void input_wait(struct display *d, uint64_t ns)
{
#ifdef __APPLE__
	struct kevent timer, got;
	EV_SET(&timer, 2, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS | NOTE_CRITICAL, (intptr_t)ns, NULL);
	d->waiting = true;
	pthread_mutex_unlock(&host.lock);
	if (d->kq >= 0)
		kevent(d->kq, &timer, 1, &got, 1, NULL);
	pthread_mutex_lock(&host.lock);
	d->waiting = false;
#else
	(void)d;
	uint64_t at = clock_ns(CLOCK_MONOTONIC) + ns;   /* (host.cond is on the monotonic clock) */
	struct timespec abs = { (time_t)(at / 1000000000u), (long)(at % 1000000000u) };
	pthread_cond_timedwait(&host.cond, &host.lock, &abs);
#endif
}

/* (host.lock held) */
static void signal_input(struct display *d)
{
#ifdef __APPLE__
	if (d->waiting && d->kq >= 0) {
		struct kevent e;
		EV_SET(&e, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
		kevent(d->kq, &e, 1, NULL, 0, NULL);
	}
#else
	(void)d;
#endif
	pthread_cond_broadcast(&host.cond);
}

/*
 * Wakes the window's thread, which waits for SDL's events, with one of its
 * own. (The window stays while the machine's thread is in the functions
 * below, other than display_close(): it goes only when that thread is done
 * or asks it to, and they wake it with host.lock held.)
 */
static void wake(struct display *d)
{
	if (atomic_exchange(&d->woken, true))
		return;
	SDL_Event e;
	memset(&e, 0, sizeof e);
	e.type = SDL_USEREVENT;
	d->sdl.PushEvent(&e);
}

void display_times(struct display *d, struct display_times *t)
{
	pthread_mutex_lock(&host.lock);
	*t = d->times_out;
	d->times_read = true;
	pthread_mutex_unlock(&host.lock);
}

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

static void emit(struct display *d, const struct display_event *ev, char *path, unsigned prompt);

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
	s->lib = hostlib_open(HOSTLIB_SDL2);
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
	SYM(PushEvent, "SDL_PushEvent");
	SYM(ShowCursor, "SDL_ShowCursor");
	SYM(SetRelativeMouseMode, "SDL_SetRelativeMouseMode");
	SYM(SetWindowTitle, "SDL_SetWindowTitle");
	SYM(SetWindowFullscreen, "SDL_SetWindowFullscreen");
	SYM(SetWindowSize, "SDL_SetWindowSize");
	SYM(GetWindowSize, "SDL_GetWindowSize");
	SYM(SetWindowPosition, "SDL_SetWindowPosition");
	SYM(GetWindowPosition, "SDL_GetWindowPosition");
	SYM(ShowWindow, "SDL_ShowWindow");
	SYM(GetWindowFlags, "SDL_GetWindowFlags");
	SYM(GetWindowDisplayIndex, "SDL_GetWindowDisplayIndex");
	SYM(GetDisplayBounds, "SDL_GetDisplayBounds");
	SYM(GetVersion, "SDL_GetVersion");
	SYM(GetCurrentVideoDriver, "SDL_GetCurrentVideoDriver");
	SYM(free, "SDL_free");
#undef SYM
	*(void **)&s->GetDisplayUsableBounds = dlsym(s->lib, "SDL_GetDisplayUsableBounds");
	*(void **)&s->GetWindowBordersSize = dlsym(s->lib, "SDL_GetWindowBordersSize");
	*(void **)&s->GetDisplayDPI = dlsym(s->lib, "SDL_GetDisplayDPI");
	*(void **)&s->ComposeCustomBlendMode = dlsym(s->lib, "SDL_ComposeCustomBlendMode");
	*(void **)&s->GetClipboardText = dlsym(s->lib, "SDL_GetClipboardText");
	*(void **)&s->SetClipboardText = dlsym(s->lib, "SDL_SetClipboardText");
	*(void **)&s->free = dlsym(s->lib, "SDL_free");
	*(void **)&s->CreateRGBSurfaceFrom = dlsym(s->lib, "SDL_CreateRGBSurfaceFrom");
	*(void **)&s->SetWindowIcon = dlsym(s->lib, "SDL_SetWindowIcon");
	*(void **)&s->FreeSurface = dlsym(s->lib, "SDL_FreeSurface");
	return true;
}

/* Blue's icon for the window (on macOS, the Dock's too) */
static void set_icon(struct display *d)
{
	struct sdl *s = &d->sdl;
	uint32_t *pixels = malloc(ICON_SIZE * ICON_SIZE * sizeof *pixels);
	if (pixels && s->CreateRGBSurfaceFrom && s->SetWindowIcon && s->FreeSurface && icon_argb(pixels)) {
		void *icon = s->CreateRGBSurfaceFrom(pixels, ICON_SIZE, ICON_SIZE, 32, ICON_SIZE * 4, 0x00FF0000u,
						     0x0000FF00u, 0x000000FFu, 0xFF000000u);
		if (icon) {
			s->SetWindowIcon(d->window, icon);      /* (it keeps a copy) */
			s->FreeSurface(icon);
		}
	}
	free(pixels);
}

struct display_view display_view(unsigned screen_w, unsigned screen_h, int out_w, int out_h, bool integer,
				 unsigned scale)
{
	struct display_view v = { .screen_w = screen_w, .screen_h = screen_h };
	if (!screen_w || !screen_h || out_w <= 0 || out_h <= 0)
		return v;
	unsigned n = (unsigned)out_w / screen_w < (unsigned)out_h / screen_h ? (unsigned)out_w / screen_w
									    : (unsigned)out_h / screen_h;
	int spare_w = out_w - (int)(n * screen_w), spare_h = out_h - (int)(n * screen_h);
	/* (a whole multiple is exact: worth leaving a pixel or two unfilled for, and what rounding left a screen the
	 * window's own at its scale) */
	bool own = n == scale && spare_w < 8 * (int)n && spare_h < (int)n;
	if (n >= 1 && (integer || own || (spare_w < spare_h ? spare_w : spare_h) <= 2)) {
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

#define DENSITY_TARGET 80.0             /* pixels per inch: a 1999 Mac's screen (Auto's aim) */
#define DENSITY_MIN 20.0                /* ... and densities out of reason */
#define DENSITY_MAX 1000.0

unsigned display_auto_scale(double ppi, double backing)
{
	unsigned s = 1;
	if (ppi >= DENSITY_MIN && ppi <= DENSITY_MAX) {
		/* (s rather than s + 1 up to 80 sqrt(s (s + 1)), where they're as far off in proportion) */
		while (s < DISPLAY_SCALE_MAX && ppi > DENSITY_TARGET * sqrt((double)s * (s + 1)))
			s++;
		return s;
	}
	return backing < 1.5 ? 1 : backing >= DISPLAY_SCALE_MAX ? DISPLAY_SCALE_MAX : (unsigned)(backing + 0.5);
}

void display_screen_for(int w, int h, unsigned scale, int *sw, int *sh)
{
	int s = scale ? (int)scale : 1;
	*sw = w > 0 ? w / s : 0;
	*sh = h > 0 ? h / s : 0;
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

/* The screen a window wh units high, pw x ph pixels, shows at the scale (above the status bar) */
static void screen_for_window(const struct display *d, int wh, int pw, int ph, int *sw, int *sh)
{
	display_screen_for(pw, ph - bar_pixels(d, wh, ph), d->scale, sw, sh);
}

/*
 * The room for the window on its display (in units: the display's usable
 * part, but the window's frame; where the window system doesn't say what
 * that is, a title bar is left room for)
 */
#define TITLE_BAR_GUESS 32              /* (macOS 27's) */

static bool window_room(struct display *d, SDL_Rect *r, int *top, int *left, int *bottom, int *right)
{
	*top = *left = *bottom = *right = 0;
	if (d->display < 0 || !d->sdl.GetDisplayUsableBounds || d->sdl.GetDisplayUsableBounds(d->display, r) != 0 ||
	    r->w <= 0 || r->h <= 0)
		return false;
	if (!d->sdl.GetWindowBordersSize || d->sdl.GetWindowBordersSize(d->window, top, left, bottom, right) != 0) {
		*left = *bottom = *right = 0;
		*top = TITLE_BAR_GUESS;
	}
	return r->w - *left - *right > 0 && r->h - *top - *bottom > bar_units(d);
}

/*
 * ... and the window that shows a screen at the scale: within the display,
 * keeping the screen's shape (with integer scaling, at the largest whole
 * multiple that fits, if one does)
 */
static void window_for_screen(struct display *d, unsigned sw, unsigned sh, int *ww, int *wh)
{
	double k = d->density > 0 ? d->density : 1, w = sw * d->scale / k, h = sh * d->scale / k;
	SDL_Rect r;
	int top, left, bottom, right;
	if (window_room(d, &r, &top, &left, &bottom, &right)) {
		double room_w = r.w - left - right, room_h = r.h - top - bottom - bar_units(d);
		if (w > room_w || h > room_h) {
			double f = fmin(room_w / w, room_h / h);
			for (unsigned n = d->scale - 1; d->integer && n >= 1; n--) {
				if (sw * n / k <= room_w && sh * n / k <= room_h) {
					f = (double)n / d->scale;
					break;
				}
			}
			w *= f;
			h *= f;
		}
	}
	*ww = (int)ceil(w - 0.01);
	*wh = (int)ceil(h - 0.01) + bar_units(d);
}

/* A window we've resized, ww x wh units, stays on its display (in its usable part, as far as it goes) */
static void keep_on_display(struct display *d, int ww, int wh)
{
	SDL_Rect r;
	int top, left, bottom, right, x, y;
	if ((d->sdl.GetWindowFlags(d->window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED)) ||
	    !window_room(d, &r, &top, &left, &bottom, &right))
		return;
	d->sdl.GetWindowPosition(d->window, &x, &y);
	int nx = x + ww + right > r.x + r.w ? r.x + r.w - right - ww : x;
	int ny = y + wh + bottom > r.y + r.h ? r.y + r.h - bottom - wh : y;
	nx = nx - left < r.x ? r.x + left : nx;
	ny = ny - top < r.y ? r.y + top : ny;
	if (nx != x || ny != y)
		d->sdl.SetWindowPosition(d->window, nx, ny);
}

/*
 * The window takes the screen's size at the scale, unless it is that size
 * already (to the 8 pixels screens sized to the window are rounded to): it
 * may be being resized.
 */
static void fit_window(struct display *d)
{
	int ww, wh, pw, ph, sw, sh;
	window_size(d, &ww, &wh, &pw, &ph);
	screen_for_window(d, wh, pw, ph, &sw, &sh);
	if (sw >= (int)d->width && sw < (int)d->width + 8 && sh == (int)d->height)
		return;
	window_for_screen(d, d->width, d->height, &ww, &wh);
	d->sdl.SetWindowSize(d->window, ww, wh);
	keep_on_display(d, ww, wh);
}

/* The display's pixels per inch (display k, at b in units, `density` pixels each): 0 if not known */
static double display_ppi(struct display *d, int k, const SDL_Rect *b, double density)
{
	unsigned px_w = (unsigned)lround(b->w * density), px_h = (unsigned)lround(b->h * density);
#ifndef __APPLE__
	/* SDL 2's own, from the X server's or the compositor's physical size (sdl2-compat's is made up: 96 a unit) */
	float h = 0, v = 0;
	if (!d->compat && d->sdl.GetDisplayDPI && d->sdl.GetDisplayDPI(k, NULL, &h, &v) == 0 && h >= DENSITY_MIN &&
	    h <= DENSITY_MAX && fabs(h / v - 1) < 0.1)
		return h;
	/* (the kernel's EDID is this computer's displays': not an X server's elsewhere) */
	const char *driver = d->sdl.GetCurrentVideoDriver(), *x = getenv("DISPLAY");
	if (!driver || !(strcmp(driver, "wayland") == 0 || (strcmp(driver, "x11") == 0 && x && x[0] == ':')))
		return 0;
#else
	(void)d;
	(void)k;
#endif
	return density_display(b->x + b->w / 2, b->y + b->h / 2, px_w, px_h);
}

/*
 * The window's density, and the display it's on: Auto's scale there, and
 * the scale in use. True if the density or the scale changed.
 */
static bool check_display(struct display *d)
{
	int ww, wh, pw, ph;
	double density = window_size(d, &ww, &wh, &pw, &ph);
	int k = d->sdl.GetWindowDisplayIndex(d->window);
	SDL_Rect b = { 0, 0, 0, 0 };
	if (k < 0 || d->sdl.GetDisplayBounds(k, &b) != 0)
		k = -1;
	d->recheck = false;
	d->checked_ns = clock_ns(CLOCK_MONOTONIC);
	bool moved = k != d->display || memcmp(&b, &d->display_bounds, sizeof b) != 0;
	bool denser = fabs(density - d->density) > 0.02;
	if (!moved && !denser)
		return false;
	unsigned was_auto = d->scale_auto, was = d->scale;
	double was_ppi = d->ppi;
	d->display = k;
	d->display_bounds = b;
	d->density = density;
	d->ppi = k >= 0 ? display_ppi(d, k, &b, density) : 0;
	d->scale_auto = display_auto_scale(d->ppi, density);
	d->scale = d->scale_chosen == DISPLAY_SCALE_OWN ? display_auto_scale(0, density) :
		   d->scale_chosen ? d->scale_chosen : d->scale_auto;
	if (d->scale_auto != was_auto || fabs(d->ppi - was_ppi) >= 0.5) {
		if (d->ppi > 0)
			printf("display: %.0f pixels per inch: Auto's scale is %u\n", d->ppi, d->scale_auto);
		else
			printf("display: its pixels per inch aren't known: Auto's scale is %u, its own\n", d->scale_auto);
	}
	return denser || d->scale != was;
}

/*
 * The scale or the window's density changed: the screen follows the
 * window if it does (the size it shows at the scale is asked for);
 * otherwise the window takes the screen's size at it.
 */
static void rescale(struct display *d)
{
	if (d->follows) {
		int ww, wh, pw, ph;
		struct display_event ev = { .type = DISPLAY_EVENT_RESIZE };
		window_size(d, &ww, &wh, &pw, &ph);
		screen_for_window(d, wh, pw, ph, &ev.x, &ev.y);
		emit(d, &ev, NULL, 0);
	} else {
		fit_window(d);
	}
	d->stale = true;
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
	/* (the format renderers take as it is: written straight into, it's uploaded without converting) */
	d->texture = make_texture(d, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height, false);
	if (!d->texture)
		return false;
	d->sdl.SetTextureBlendMode(d->texture, SDL_BLENDMODE_NONE);
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
		d->scaled_from = 0;
		d->scaled_to = d->height;
	}
	if (d->scaled_from >= d->scaled_to)
		return d->scaled;
	if (d->sdl.SetRenderTarget(d->renderer, d->scaled) != 0)
		return NULL;
	SDL_Rect from = { 0, (int)d->scaled_from, (int)d->width, (int)(d->scaled_to - d->scaled_from) };
	SDL_Rect to = { 0, from.y * (int)k, from.w * (int)k, from.h * (int)k };
	d->sdl.RenderCopy(d->renderer, d->texture, &from, &to);
	d->sdl.SetRenderTarget(d->renderer, NULL);
	d->scaled_from = d->scaled_to = 0;
	return d->scaled;
}

/* Rows of the screen changed: `scaled` needs them drawn again */
static void scaled_stale(struct display *d, unsigned from, unsigned to)
{
	if (d->scaled_from >= d->scaled_to) {
		d->scaled_from = from;
		d->scaled_to = to;
	} else {
		d->scaled_from = from < d->scaled_from ? from : d->scaled_from;
		d->scaled_to = to > d->scaled_to ? to : d->scaled_to;
	}
}

/* Where the screen is drawn: in the window above the status bar */
static struct display_view screen_view(struct display *d, int *ww, int *wh, int *pw, int *ph)
{
	window_size(d, ww, wh, pw, ph);
	return display_view(d->width, d->height, *pw, *ph - bar_pixels(d, *wh, *ph), d->integer, d->scale);
}

/*
 * The pointer's textures, at k times its size (filtered, when drawn at other
 * sizes): its pixels, and white where it inverts the screen, for a blend
 * mode that does (where the renderer has none, those pixels are black).
 */
static bool cursor_textures(struct display *d, unsigned k, bool filtered)
{
	if (d->cursor_color && d->cursor_image == d->cursor_images && d->cursor_scale == k &&
	    d->cursor_filtered == filtered)
		return true;
	if (d->cursor_color)
		d->sdl.DestroyTexture(d->cursor_color);
	if (d->cursor_invert)
		d->sdl.DestroyTexture(d->cursor_invert);
	d->cursor_color = d->cursor_invert = NULL;
	unsigned w = d->cursor.width * k, h = d->cursor.height * k;
	uint32_t *color = malloc((size_t)w * h * 8), *invert = color + (size_t)w * h;
	if (!color)
		return false;
	bool inverts = false;
	for (unsigned y = 0; y < h; y++) {
		for (unsigned x = 0; x < w; x++) {
			bool inv = d->cursor_pixels[y / k * d->cursor.width + x / k] == DISPLAY_CURSOR_INVERT;
			invert[y * w + x] = inv ? 0xFFFFFFFFu : 0;
			inverts |= inv;
		}
	}
	if (inverts && d->invert_mode >= 0) {
		d->cursor_invert = make_texture(d, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h, filtered);
		if (d->cursor_invert && d->sdl.SetTextureBlendMode(d->cursor_invert, d->invert_mode) != 0) {
			d->sdl.DestroyTexture(d->cursor_invert);        /* (the renderer can't: SDL's software one) */
			d->cursor_invert = NULL;
			d->invert_mode = -1;
		}
		if (d->cursor_invert)
			d->sdl.UpdateTexture(d->cursor_invert, NULL, invert, (int)w * 4);
	}
	for (unsigned y = 0; y < h; y++) {
		for (unsigned x = 0; x < w; x++) {
			uint32_t px = d->cursor_pixels[y / k * d->cursor.width + x / k];
			color[y * w + x] = px != DISPLAY_CURSOR_INVERT ? px : d->cursor_invert ? DISPLAY_CURSOR_CLEAR : 0xFF000000u;
		}
	}
	d->cursor_color = make_texture(d, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h, filtered);
	if (d->cursor_color) {
		d->sdl.UpdateTexture(d->cursor_color, NULL, color, (int)w * 4);
		d->sdl.SetTextureBlendMode(d->cursor_color, SDL_BLENDMODE_BLEND);
	}
	free(color);
	d->cursor_image = d->cursor_images;
	d->cursor_scale = k;
	d->cursor_filtered = filtered;
	return d->cursor_color != NULL;
}

/* The pointer over the screen (drawn at v, from `scaled` at k times its size if `filtered`) */
static void draw_cursor(struct display *d, const struct display_view *v, unsigned k, bool filtered)
{
	const struct display_cursor *c = &d->cursor;
	if (!c->visible || v->w <= 0 || v->h <= 0)
		return;
	/* (the part on the screen, in its pixels) */
	int x0 = c->x > 0 ? c->x : 0, y0 = c->y > 0 ? c->y : 0;
	int x1 = c->x + (int)c->width, y1 = c->y + (int)c->height;
	x1 = x1 < (int)d->width ? x1 : (int)d->width;
	y1 = y1 < (int)d->height ? y1 : (int)d->height;
	if (x0 >= x1 || y0 >= y1 || !cursor_textures(d, filtered ? k : 1, filtered))
		return;
	int n = (int)d->cursor_scale;
	SDL_Rect src = { (x0 - c->x) * n, (y0 - c->y) * n, (x1 - x0) * n, (y1 - y0) * n };
	int dx0 = (int)lround((double)x0 * v->w / d->width), dx1 = (int)lround((double)x1 * v->w / d->width);
	int dy0 = (int)lround((double)y0 * v->h / d->height), dy1 = (int)lround((double)y1 * v->h / d->height);
	SDL_Rect dst = { v->x + dx0, v->y + dy0, dx1 - dx0, dy1 - dy0 };
	if (d->cursor_invert)
		d->sdl.RenderCopy(d->renderer, d->cursor_invert, &src, &dst);
	d->sdl.RenderCopy(d->renderer, d->cursor_color, &src, &dst);
}

static void flush(struct display *d);

/* Draws the window: the screen, the pointer, the status bar, the menu or a prompt */
static void draw(struct display *d)
{
	uint64_t cpu = clock_ns(CLOCK_THREAD_CPUTIME_ID), t = clock_ns(CLOCK_MONOTONIC);
	int ww, wh, pw, ph;
	struct display_view v = screen_view(d, &ww, &wh, &pw, &ph);
	unsigned k = v.prescale, max = (unsigned)d->max_texture;
	while (k > 1 && max && (d->width * k > max || d->height * k > max))
		k--;
	void *src = k && d->prescale ? prescaled(d, k) : NULL;
	SDL_Rect dst = { v.x, v.y, v.w, v.h };
	d->sdl.RenderClear(d->renderer);
	if (d->texture)
		d->sdl.RenderCopy(d->renderer, src ? src : d->texture, NULL, &dst);
	draw_cursor(d, &v, k, src != NULL);
	if (d->ui && (d->menu || d->status_bar || d->prompt)) {
		double now = (double)t * 1e-9;
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
			.prompt_alt = d->prompt_alt[0] ? d->prompt_alt : NULL,
			.drives = d->has_drives ? &d->drives : NULL,
			.tools = d->tools,
			.tools_why_not = d->tools_why_not[0] ? d->tools_why_not : NULL,
			.scale = d->scale_chosen,
			.scale_auto = d->scale_auto,
		};
		enum ui_action action = ui_draw(d->ui, &s, ww, wh, ww > 0 ? (float)pw / (float)ww : 1, now - d->menu_time);
		d->menu_time = now;
		if (action != UI_NONE)
			d->choice = action;
	}
	d->stale = false;
	t = timed(d, DISPLAY_TIME_DRAW, t, &cpu);
	d->sdl.RenderPresent(d->renderer);
	timed(d, DISPLAY_TIME_PRESENT, t, &cpu);
	d->times.draws++;
}

/* An event for the machine (path: a dropped file's, the machine's to free; prompt: the one it answers) */
static void emit(struct display *d, const struct display_event *ev, char *path, unsigned prompt)
{
	if (d->nout == OUT)
		flush(d);
	d->out[d->nout++] = (struct queued){ *ev, path, prompt };
}

/* The events gathered, to the machine's queue (one move after another is one move) */
static void flush(struct display *d)
{
	if (!d->nout)
		return;
	pthread_mutex_lock(&host.lock);
	for (unsigned k = 0; k < d->nout; k++) {
		struct queued *q = &d->out[k];
		struct queued *last = d->count ? &d->queue[(d->head + d->count - 1) % QUEUE] : NULL;
		if (q->ev.type == DISPLAY_EVENT_MOUSE_MOVE && last && last->ev.type == DISPLAY_EVENT_MOUSE_MOVE) {
			last->ev.x = q->ev.x;
			last->ev.y = q->ev.y;
			last->ev.dx += q->ev.dx;
			last->ev.dy += q->ev.dy;
			continue;
		}
		if (q->ev.type == DISPLAY_EVENT_MENU_ACTION && q->prompt && q->prompt == d->in.prompt_id)
			d->in.prompt = false;           /* (answered) */
		if (d->count == QUEUE) {                /* (the machine isn't taking them) */
			free(q->path);
			continue;
		}
		d->queue[(d->head + d->count++) % QUEUE] = *q;
	}
	d->nout = 0;
	signal_input(d);
	pthread_mutex_unlock(&host.lock);
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

/* The window's title: what the pointer does, or that the guest's paused (without the menu's SDL) */
static void set_title(struct display *d)
{
	d->sdl.SetWindowTitle(d->window, d->menu && !d->ui ? "Blue (paused: F12 resumes)" :
				    d->captured ? "Blue (Control-Option releases the mouse)" : "Blue");
}

/* The host's pointer is shown for the menu and prompts (the guest's doesn't move meanwhile) */
static void show_pointer(struct display *d)
{
	d->sdl.ShowCursor((d->menu || d->prompt) && d->ui ? 1 : 0);
}

/*
 * Shows or hides the status bar. The window grows or shrinks to keep the
 * screen's size (as far as the display has room), unless it keeps its size
 * (fullscreen, maximized): then it's the screen that fits it that changes,
 * as if the user had resized it.
 */
static void show_status_bar(struct display *d, bool on)
{
	if (on == d->status_bar || (on && !d->ui))
		return;
	int ww, wh, pw, ph;
	d->sdl.GetWindowSize(d->window, &ww, &wh);
	d->status_bar = on;
	if (d->sdl.GetWindowFlags(d->window) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED)) {
		struct display_event ev = { .type = DISPLAY_EVENT_RESIZE };
		window_size(d, &ww, &wh, &pw, &ph);
		screen_for_window(d, wh, pw, ph, &ev.x, &ev.y);
		emit(d, &ev, NULL, 0);
	} else {
		int nw = ww, nh = wh + (on ? UI_STATUS_BAR_HEIGHT : -UI_STATUS_BAR_HEIGHT), top, left, bottom, right;
		SDL_Rect r;
		if (on && window_room(d, &r, &top, &left, &bottom, &right) && nh > r.h - top - bottom && wh > 0) {
			/* (it would go off the display: the screen's part smaller instead, keeping its shape) */
			int room = r.h - top - bottom - UI_STATUS_BAR_HEIGHT;
			nw = (int)lround((double)ww * room / wh);
			nh = room + UI_STATUS_BAR_HEIGHT;
		}
		d->sdl.SetWindowSize(d->window, nw, nh);
	}
	d->stale = true;
}

static void chooser_wake(void *arg)
{
	wake(arg);
}

/*
 * A drive's Insert...: the host's file chooser (one at a time), whose file
 * the machine puts in that drive as if dropped (insert_chosen()). Without
 * one, the machine's told so (no path): it says to drop an image instead.
 */
static void insert(struct display *d, unsigned k)
{
	char name[24], message[80];
	if (d->chooser)
		return;
	snprintf(name, sizeof name, "%s", k < d->drives.count ? d->drives.drive[k].name : "drive");
	if (name[0] >= 'A' && name[0] <= 'Z' && name[1] >= 'a' && name[1] <= 'z')
		name[0] = (char)(name[0] - 'A' + 'a');          /* ("floppy drive"; "CD-ROM drive" stays) */
	snprintf(message, sizeof message, "Choose a disk image to put in the %s", name);
	d->chooser = chooser_open(message, chooser_wake, d);
	d->chooser_drive = k;
	if (!d->chooser) {
		struct display_event ev = { .type = DISPLAY_EVENT_MENU_ACTION, .action = (enum ui_action)(UI_INSERT + k) };
		emit(d, &ev, NULL, 0);
	}
}

/* ... and the file chosen (or that it went wrong), once the chooser's done */
static void insert_chosen(struct display *d)
{
	char *path;
	enum chooser_state state = chooser_poll(d->chooser, &path);
	if (state == CHOOSER_WAITING)
		return;
	d->chooser = NULL;
	if (state != CHOOSER_CANCELLED) {
		struct display_event ev = { .type = DISPLAY_EVENT_MENU_ACTION,
					    .action = (enum ui_action)(UI_INSERT + d->chooser_drive) };
		emit(d, &ev, path, 0);
	}
}

/*
 * What was chosen in the menu or a prompt: fullscreen, the status bar and
 * the scale are the window's, Insert... its chooser
 */
static void chosen(struct display *d, enum ui_action action)
{
	struct display_event ev = { .type = DISPLAY_EVENT_MENU_ACTION, .action = action };
	unsigned prompt = 0;
	if (action >= UI_INSERT && action <= UI_INSERT_LAST) {
		insert(d, (unsigned)(action - UI_INSERT));
		return;
	}
	if (action >= UI_SCALE && action <= UI_SCALE_LAST) {
		d->scale_chosen = (unsigned)(action - UI_SCALE);
		unsigned scale = d->scale_chosen ? d->scale_chosen : d->scale_auto;
		if (scale != d->scale) {
			d->scale = scale;
			rescale(d);
		}
		d->stale = true;
		return;
	}
	switch (action) {
	case UI_FULLSCREEN:
		d->fullscreen = !d->fullscreen;
		d->sdl.SetWindowFullscreen(d->window, d->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
		d->stale = true;
		return;
	case UI_STATUS_BAR:
		show_status_bar(d, !d->status_bar);
		return;
	case UI_RESUME:
		ev.type = DISPLAY_EVENT_MENU;
		break;
	case UI_PROMPT_YES:
	case UI_PROMPT_NO:
	case UI_PROMPT_ALT:
		d->prompt = false;
		prompt = d->prompt_id;
		show_pointer(d);
		d->stale = true;
		break;
	default:
		break;
	}
	emit(d, &ev, NULL, prompt);
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

/* An SDL event: for the machine (as events it takes), the menu or a prompt, or the window itself */
static void handle(struct display *d, SDL_Event *e)
{
	struct display_event ev = { .type = DISPLAY_EVENT_NONE };
	switch (e->type) {
	case SDL_QUIT:                  /* (the close button, Quit in the menu bar or the Dock: see catch_signals()) */
		ev.type = DISPLAY_EVENT_CLOSE;
		break;
	case SDL_RENDER_TARGETS_RESET:
		scaled_stale(d, 0, d->height);
		d->stale = true;
		return;
	case SDL_CLIPBOARDUPDATE:
		ev.type = DISPLAY_EVENT_CLIPBOARD;
		break;
	case SDL_WINDOWEVENT:
		if (e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
			ev.type = DISPLAY_EVENT_CLIPBOARD;
			break;
		}
		if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
			/* (keys let go elsewhere aren't seen) */
			d->ctrl[0] = d->ctrl[1] = d->alt[0] = d->alt[1] = false;
			return;
		}
		/* (perhaps to another display, or of another density) */
		if (e->window.event == SDL_WINDOWEVENT_SHOWN || e->window.event == SDL_WINDOWEVENT_MOVED ||
		    e->window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED || e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
			d->recheck = true;
		/* (some compositors, Wayland's, resize the window only as fast as it's drawn) */
		if (e->window.event == SDL_WINDOWEVENT_EXPOSED || e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
			d->stale = true;
			return;
		}
		if (e->window.event == SDL_WINDOWEVENT_RESIZED) {
			int ww, wh, pw, ph;
			double density = window_size(d, &ww, &wh, &pw, &ph);
			ev.type = DISPLAY_EVENT_RESIZE;
			screen_for_window(d, e->window.data2, (int)lround(e->window.data1 * density),
					  (int)lround(e->window.data2 * density), &ev.x, &ev.y);
		}
		break;
	case SDL_KEYDOWN:
	case SDL_KEYUP: {
		bool down = e->type == SDL_KEYDOWN;
		if (e->key.repeat)
			return;
		unsigned sc = (unsigned)e->key.scancode;
		/* F12 opens and closes the menu (Esc closes it too): the guest doesn't get them */
		if (sc == SDL_SCANCODE_F12 || (d->menu && sc == SDL_SCANCODE_ESCAPE)) {
			if (down)
				ev.type = DISPLAY_EVENT_MENU;
			break;
		}
		if (d->menu) {
			menu_key(d, sc, down);
			d->stale = true;
			return;
		}
		if (d->prompt) {                /* (Esc is its "no") */
			d->stale = true;
			if (sc != SDL_SCANCODE_ESCAPE)
				menu_key(d, sc, down);
			else if (down)
				chosen(d, d->prompt_no[0] ? UI_PROMPT_NO : UI_PROMPT_YES);
			return;
		}
		/* Control and Option (Ctrl and Alt) together: the release key (and keys to the guest too) */
		if (sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL)
			d->ctrl[sc == SDL_SCANCODE_RCTRL] = down;
		if (sc == SDL_SCANCODE_LALT || sc == SDL_SCANCODE_RALT)
			d->alt[sc == SDL_SCANCODE_RALT] = down;
		if (down && (d->ctrl[0] || d->ctrl[1]) && (d->alt[0] || d->alt[1]) &&
		    (sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL || sc == SDL_SCANCODE_LALT ||
		     sc == SDL_SCANCODE_RALT)) {
			struct display_event release = { .type = DISPLAY_EVENT_RELEASE_MOUSE };
			emit(d, &release, NULL, 0);
		}
		if (sc == SDL_SCANCODE_CAPSLOCK) {
			/* the ADB caps lock key latches: down when engaged, up when released */
			if (!down)
				return;
			d->caps_lock = !d->caps_lock;
			down = d->caps_lock;
		}
		uint8_t code = display_hid_to_adb(sc);
		if (code == 0xFF)
			return;
		ev.type = DISPLAY_EVENT_KEY;
		ev.adb_key = code;
		ev.down = down;
		break;
	}
	case SDL_MOUSEMOTION:
		if (d->menu || d->prompt) {
			if (d->ui)
				ui_mouse_move(d->ui, (float)e->motion.x, (float)e->motion.y);
			d->stale = true;
			return;
		}
		ev.type = DISPLAY_EVENT_MOUSE_MOVE;
		to_screen(d, e->motion.x, e->motion.y, &ev.x, &ev.y);
		ev.dx = e->motion.xrel;
		ev.dy = e->motion.yrel;
		break;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
		if (d->menu || d->prompt) {
			/* (SDL's buttons are left, middle, right; Dear ImGui's left, right, middle) */
			static const int buttons[4] = { -1, 0, 2, 1 };
			if (d->ui && e->button.button < 4)
				ui_mouse_button(d->ui, buttons[e->button.button], e->type == SDL_MOUSEBUTTONDOWN);
			d->stale = true;
			return;
		}
		/* (a click on the status bar isn't one at the bottom of the screen; letting go anywhere is) */
		if (e->type == SDL_MOUSEBUTTONDOWN && !d->captured && on_status_bar(d, e->button.y))
			return;
		ev.type = DISPLAY_EVENT_MOUSE_BUTTON;
		ev.button = e->button.button - 1;
		ev.down = e->type == SDL_MOUSEBUTTONDOWN;
		to_screen(d, e->button.x, e->button.y, &ev.x, &ev.y);
		break;
	case SDL_MOUSEWHEEL:
		if ((d->menu || d->prompt) && d->ui) {
			float flip = e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
			ui_mouse_wheel(d->ui, flip * (float)e->wheel.x, flip * (float)e->wheel.y);
			d->stale = true;
		}
		return;
	case SDL_DROPFILE:
	case SDL_DROPTEXT: {
		if (!e->drop.file)
			return;
		char *path = e->type == SDL_DROPFILE ? strdup(e->drop.file) : NULL;
		d->sdl.free(e->drop.file);
		if (!path)
			return;
		ev.type = DISPLAY_EVENT_DROP_FILE;
		emit(d, &ev, path, 0);
		return;
	}
	default:
		return;
	}
	if (ev.type != DISPLAY_EVENT_NONE)
		emit(d, &ev, NULL, 0);
}

/* What the window takes over (host.lock held) to act on afterwards */
struct taken {
	bool frame;                     /* a frame ended (with changes or not) */
	uint64_t ended_ns;              /* when the frame taken was handed over (0: none taken) */
	bool resized;                   /* the screen's size changed */
	unsigned width, height;
	struct video_rows bands[BANDS]; /* the rows converted, to upload */
	unsigned nbands;
	bool menu, prompt;              /* opened or closed */
	int capture, status_bar, shortcuts;
	char *clipboard;
	bool clipboard_asked;
	bool close;
};

/*
 * The screen's changed rows converted into argb (what the texture is
 * updated from), in bands to upload (host.lock held: the machine doesn't
 * touch the frame meanwhile)
 */
static void take_frame(struct display *d, struct taken *t)
{
	struct display_frame *f = &d->frame;
	unsigned w = d->frame_w, h = d->frame_h;
	if (!d->frame_ended || !w || !h || !f->palette.bpp)
		return;
	d->frame_ended = false;
	t->frame = true;
	if ((size_t)w * h > d->argb_size) {
		uint32_t *argb = realloc(d->argb, (size_t)w * h * sizeof *argb);
		if (!argb)
			return;
		d->argb = argb;
		d->argb_size = (size_t)w * h;
	}
	if (w != d->width || h != d->height) {
		t->resized = true;
		t->width = w;
		t->height = h;
	}
	uint64_t cpu = clock_ns(CLOCK_THREAD_CPUTIME_ID), t0 = clock_ns(CLOCK_MONOTONIC);
	for (unsigned y = 0; y < h;) {
		if (!f->changed[y]) {
			y++;
			continue;
		}
		unsigned to = y + 1;
		while (to < h && f->changed[to])
			to++;
		video_convert(&f->palette, f->pixels + (size_t)y * d->frame_rb, d->frame_rb, w, to - y, d->argb + (size_t)y * w,
			      (size_t)w * 4);
		memset(f->changed + y, 0, to - y);
		if (t->nbands && (y - t->bands[t->nbands - 1].to < BAND_GAP || t->nbands == BANDS))
			t->bands[t->nbands - 1].to = to;
		else
			t->bands[t->nbands++] = (struct video_rows){ y, to };
		y = to;
	}
	if (t->resized) {               /* (all of it is shown anew) */
		t->bands[0] = (struct video_rows){ 0, h };
		t->nbands = 1;
	}
	if (t->nbands) {
		d->times.frames++;
		t->ended_ns = d->frame_end_ns;
	}
	timed(d, DISPLAY_TIME_CONVERT, t0, &cpu);
}

/* What the machine handed over (host.lock held) */
static void take(struct display *d, struct taken *t)
{
	struct handed *in = &d->in;
	take_frame(d, t);
	if (in->cursor_changed) {
		const struct display_cursor *c = &in->cursor;
		if (c->visible != d->cursor.visible ||
		    (c->visible && (c->x != d->cursor.x || c->y != d->cursor.y || c->image != d->cursor.image)))
			d->stale = true;
		if (c->image != d->cursor.image) {
			memcpy(d->cursor_pixels, in->cursor_pixels, sizeof d->cursor_pixels);
			d->cursor_images++;
		}
		d->cursor = *c;
		d->cursor.pixels = d->cursor_pixels;
		in->cursor_changed = false;
	}
	if (in->bar_changed) {
		d->bar = in->bar;
		d->stale |= d->status_bar;
		in->bar_changed = false;
	}
	if (in->menu_changed) {
		t->menu = d->menu != in->menu;
		d->menu = in->menu;
		in->menu_changed = false;
	}
	if (in->info_changed) {
		d->menu_mouse = in->mouse;
		memcpy(d->menu_status, in->status, sizeof d->menu_status);
		d->has_drives = in->has_drives;
		d->drives = in->drives;
		d->tools = in->tools;
		memcpy(d->tools_why_not, in->tools_why_not, sizeof d->tools_why_not);
		d->stale |= d->menu;
		in->info_changed = false;
	}
	if (in->prompt_changed) {
		t->prompt = true;
		d->prompt = in->prompt;
		d->prompt_id = in->prompt_id;
		memcpy(d->prompt_text, in->prompt_text, sizeof d->prompt_text);
		memcpy(d->prompt_yes, in->prompt_yes, sizeof d->prompt_yes);
		memcpy(d->prompt_no, in->prompt_no, sizeof d->prompt_no);
		memcpy(d->prompt_alt, in->prompt_alt, sizeof d->prompt_alt);
		in->prompt_changed = false;
	}
	t->capture = in->capture;
	t->status_bar = in->status_bar;
	t->shortcuts = in->shortcuts;
	in->capture = in->status_bar = in->shortcuts = -1;
	if (in->follows >= 0)
		d->follows = in->follows;
	in->follows = -1;
	t->clipboard = in->clipboard;
	in->clipboard = NULL;
	t->clipboard_asked = in->clipboard_asked;
	in->clipboard_asked = false;
	t->close = in->close;
	if (d->times_read)
		d->times.latency_max_ns = 0;
	d->times_read = false;
	d->times_out = d->times;
}

/* ... and done */
static void act(struct display *d, struct taken *t)
{
	if (t->resized && set_screen_size(d, t->width, t->height))
		fit_window(d);
	uint64_t cpu = clock_ns(CLOCK_THREAD_CPUTIME_ID), t0 = clock_ns(CLOCK_MONOTONIC);
	for (unsigned k = 0; k < t->nbands && d->texture; k++) {
		const struct video_rows *b = &t->bands[k];
		SDL_Rect r = { 0, (int)b->from, (int)d->width, (int)(b->to - b->from) };
		d->sdl.UpdateTexture(d->texture, &r, d->argb + (size_t)b->from * d->width, (int)d->width * 4);
		scaled_stale(d, b->from, b->to);
		d->stale = true;
	}
	if (t->nbands)
		timed(d, DISPLAY_TIME_UPLOAD, t0, &cpu);
	if (t->menu || t->prompt) {
		d->ctrl[0] = d->ctrl[1] = d->alt[0] = d->alt[1] = false;
		show_pointer(d);
		if (t->menu)
			set_title(d);
		if (t->menu && d->menu)
			d->menu_time = (double)clock_ns(CLOCK_MONOTONIC) * 1e-9;
		d->stale = true;
	}
	if (t->capture >= 0) {
		d->captured = t->capture;
		d->sdl.SetRelativeMouseMode(t->capture);
		set_title(d);
	}
	if (t->status_bar >= 0)
		show_status_bar(d, t->status_bar);
#ifdef __APPLE__
	if (t->shortcuts >= 0)
		menubar_shortcuts(t->shortcuts);
#endif
	if (t->clipboard && d->sdl.SetClipboardText)
		d->sdl.SetClipboardText(t->clipboard);
	free(t->clipboard);
	if (t->clipboard_asked) {
		char *text = d->sdl.GetClipboardText(), *copy = text ? strdup(text) : NULL;
		d->sdl.free(text);
		pthread_mutex_lock(&host.lock);
		free(d->in.clipboard_text);
		d->in.clipboard_text = copy;
		d->in.clipboard_answered = true;
		pthread_cond_broadcast(&host.cond);
		pthread_mutex_unlock(&host.lock);
	}
}

static void destroy_window(struct display *d);

/*
 * The menu is drawn this often while it's open (the machine, paused, sends
 * no frames); a prompt with each frame the machine sends, as the screen
 * underneath (drawn more often, it took the GPU from the 3D engine's frames
 * meanwhile), and as it answers input
 */
#define REDRAW_NS 16000000u
#define IDLE_MS 250                     /* (the window looks round at least this often) */
#define CHECK_NS 1000000000u            /* (and at its display this often) */

/* The window's thread: until the machine closes the window, or is over */
static void run(struct display *d)
{
	uint64_t drawn = 0;
	for (;;) {
		bool animated = d->menu;
		uint64_t now = clock_ns(CLOCK_MONOTONIC);
		int timeout = animated ? (drawn + REDRAW_NS > now ? (int)((drawn + REDRAW_NS - now) / 1000000u) : 0) :
				IDLE_MS;
		SDL_Event e;
		uint64_t cpu = clock_ns(CLOCK_THREAD_CPUTIME_ID);
		bool got = d->sdl.WaitEventTimeout(&e, timeout) == 1;
		cpu = clock_ns(CLOCK_THREAD_CPUTIME_ID) - cpu;  /* (waiting, but for what SDL does meanwhile) */
		atomic_store(&d->woken, false);

		struct taken t = { .nbands = 0 };
		pthread_mutex_lock(&host.lock);
		take(d, &t);
		bool done = host.done || t.close;
		pthread_mutex_unlock(&host.lock);
		if (done) {
			free(t.clipboard);
			break;
		}
		act(d, &t);

		uint64_t t0 = clock_ns(CLOCK_MONOTONIC), c0 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
		if (signalled) {
			struct display_event quit = { .type = DISPLAY_EVENT_QUIT };
			signalled = 0;
			emit(d, &quit, NULL, 0);
		}
		if (got)
			handle(d, &e);
		while (d->sdl.PollEvent(&e))
			handle(d, &e);
		if (d->chooser)
			insert_chosen(d);
		/* (the display the window is on and its density: when they may have changed, and every second) */
		if ((d->recheck || t0 - d->checked_ns >= CHECK_NS) && check_display(d))
			rescale(d);
		flush(d);
		d->times.cpu_ns[DISPLAY_TIME_EVENTS] += cpu;
		timed(d, DISPLAY_TIME_EVENTS, t0, &c0);

		now = clock_ns(CLOCK_MONOTONIC);
		if ((d->menu && now - drawn >= REDRAW_NS - 1000000u) || (d->prompt && t.frame))
			d->stale = true;
		if (d->stale) {
			draw(d);
			drawn = now;
			if (t.ended_ns) {
				uint64_t latency = clock_ns(CLOCK_MONOTONIC) - t.ended_ns;
				d->times.latency_ns += latency;
				d->times.latency_max_ns = latency > d->times.latency_max_ns ? latency : d->times.latency_max_ns;
			}
		}
		if (d->choice != UI_NONE) {
			chosen(d, d->choice);
			d->choice = UI_NONE;
			flush(d);
		}
	}
	pthread_mutex_lock(&host.lock);
	host.d = NULL;
	host.closed = true;
	pthread_cond_broadcast(&host.cond);
	pthread_mutex_unlock(&host.lock);
	destroy_window(d);
}

/* ---- the window's thread: opening and closing ---------------------------------------- */

static struct display *open_window(const char *title, unsigned width, unsigned height, bool integer, unsigned scale)
{
	struct display *d = calloc(1, sizeof *d);
	if (!d)
		return NULL;
	d->kq = -1;
	if (!load_sdl(&d->sdl)) {
		snprintf(failure, sizeof failure, "SDL2 isn't there");
		goto fail;
	}
	/* sdl2-compat (SDL2 on SDL3) makes a window's units pixels on Wayland, unless told
	 * the program handles high-density displays itself, as this one does */
	if (!getenv(SDL_HINT_WAYLAND_SCALE_TO_DISPLAY))
		d->sdl.SetHintWithPriority(SDL_HINT_WAYLAND_SCALE_TO_DISPLAY, "0", SDL_HINT_OVERRIDE);
	/* the window's class on X11 and app ID on Wayland, as art/blue.desktop has them (SDL 2 reads them
	 * from the environment, SDL 3 from a hint), and the name sound servers and the like show */
	setenv("SDL_VIDEO_X11_WMCLASS", "blue", 0);
	setenv("SDL_VIDEO_WAYLAND_WMCLASS", "blue", 0);
	d->sdl.SetHintWithPriority("SDL_APP_ID", "blue", SDL_HINT_NORMAL);
	d->sdl.SetHintWithPriority("SDL_APP_NAME", "Blue", SDL_HINT_NORMAL);
	catch_signals(true);
	if (d->sdl.Init(SDL_INIT_VIDEO) != 0) {
		snprintf(failure, sizeof failure, "%s", d->sdl.GetError());
		catch_signals(false);
		goto fail;
	}
#ifdef __APPLE__
	menubar_install();
#endif
	d->integer = integer;
	d->scale_chosen = scale <= DISPLAY_SCALE_OWN ? scale : DISPLAY_SCALE_AUTO;
	d->display = -1;
	SDL_version v = { 0, 0, 0 };
	d->sdl.GetVersion(&v);
	d->compat = v.major == 2 && v.patch >= 50;      /* (sdl2-compat's versions: 2.x.50 and on) */
	/* (on high-density displays, drawn at their full resolution rather than scaled up by the system;
	 * shown once it's the screen's size at the scale) */
	d->window = d->sdl.CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int)width, (int)height,
					SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN);
	if (d->window) {
		set_icon(d);
		d->renderer = d->sdl.CreateRenderer(d->window, -1, 0);
	}
	if (!d->renderer || !set_screen_size(d, width, height)) {
		snprintf(failure, sizeof failure, "%s", d->sdl.GetError());
		destroy_window(d);
		return NULL;
	}
	SDL_RendererInfo info;
	if (d->sdl.GetRendererInfo(d->renderer, &info) == 0) {
		/* (in software, drawing the screen at several times its size each frame costs too much) */
		d->prescale = (info.flags & SDL_RENDERER_TARGETTEXTURE) && !(info.flags & SDL_RENDERER_SOFTWARE);
		d->max_texture = info.max_texture_width < info.max_texture_height ? info.max_texture_width
										  : info.max_texture_height;
	}
	d->invert_mode = -1;
	if (d->sdl.ComposeCustomBlendMode)      /* (the screen made white where the source is, kept where it's black) */
		d->invert_mode = d->sdl.ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE_MINUS_DST_COLOR,
			SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR, SDL_BLENDOPERATION_ADD, SDL_BLENDFACTOR_ZERO,
			SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD);
	check_display(d);
	fit_window(d);
	if (d->display >= 0)
		d->sdl.SetWindowPosition(d->window, SDL_WINDOWPOS_CENTERED_DISPLAY(d->display),
					 SDL_WINDOWPOS_CENTERED_DISPLAY(d->display));
	d->sdl.ShowWindow(d->window);
	d->sdl.ShowCursor(0);           /* Mac OS draws its own */
	d->ui = open_menu(d);
	d->in.capture = d->in.status_bar = d->in.follows = d->in.shortcuts = -1;
	d->stale = true;
#ifdef __APPLE__
	struct kevent e;
	EV_SET(&e, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
	d->kq = kqueue();
	if (d->kq >= 0 && kevent(d->kq, &e, 1, NULL, 0, NULL) != 0) {
		close(d->kq);
		d->kq = -1;
	}
#endif
	return d;
fail:
	if (d->sdl.lib)
		dlclose(d->sdl.lib);
	free(d);
	return NULL;
}

/* (the machine is done with it, or waits in display_close()) */
static void destroy_window(struct display *d)
{
	if (d->chooser)
		chooser_close(d->chooser);
	for (unsigned k = 0; k < d->count; k++)
		free(d->queue[(d->head + k) % QUEUE].path);
	for (unsigned k = 0; k < d->nout; k++)
		free(d->out[k].path);
	free(d->in.clipboard);
	free(d->in.clipboard_text);
	if (d->kq >= 0)
		close(d->kq);
	ui_close(d->ui);
	if (d->cursor_color)
		d->sdl.DestroyTexture(d->cursor_color);
	if (d->cursor_invert)
		d->sdl.DestroyTexture(d->cursor_invert);
	if (d->scaled)
		d->sdl.DestroyTexture(d->scaled);
	if (d->texture)
		d->sdl.DestroyTexture(d->texture);
	if (d->renderer)
		d->sdl.DestroyRenderer(d->renderer);
	if (d->window)
		d->sdl.DestroyWindow(d->window);
	d->sdl.Quit();
	catch_signals(false);
	dlclose(d->sdl.lib);
	free(d->argb);
	free(d->frame.pixels);
	free(d->frame.changed);
	free(d);
}

/* ---- the machine's thread ---------------------------------------------------------- */

struct body {
	int (*fn)(void *);
	void *arg;
	int ret;
};

static void *machine(void *p)
{
	struct body *b = p;
	b->ret = b->fn(b->arg);
	pthread_mutex_lock(&host.lock);
	host.done = true;
	if (host.d)
		wake(host.d);
	pthread_cond_broadcast(&host.cond);
	pthread_mutex_unlock(&host.lock);
	return NULL;
}

/* (as much stack as the main thread has: at least 8 MB) */
static size_t stack_size(void)
{
	struct rlimit r;
	size_t size = 8u << 20;
	if (getrlimit(RLIMIT_STACK, &r) == 0 && r.rlim_cur != RLIM_INFINITY && r.rlim_cur > size)
		size = r.rlim_cur < (256u << 20) ? (size_t)r.rlim_cur : 256u << 20;
	return size;
}

int display_main(int (*fn)(void *), void *arg)
{
	struct body b = { fn, arg, 0 };
	pthread_condattr_t ca;
	pthread_condattr_init(&ca);
#ifndef __APPLE__
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
#endif
	pthread_cond_init(&host.cond, &ca);
	pthread_condattr_destroy(&ca);
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, stack_size());
	pthread_t thread;
	host.running = true;
	if (pthread_create(&thread, &attr, machine, &b) != 0) {
		host.running = false;
		pthread_attr_destroy(&attr);
		return fn(arg);         /* (without a window) */
	}
	pthread_attr_destroy(&attr);
	pthread_mutex_lock(&host.lock);
	while (!host.done) {
		if (!host.opening) {
			pthread_cond_wait(&host.cond, &host.lock);
			continue;
		}
		host.opening = false;
		const char *title = host.title;
		unsigned width = host.width, height = host.height;
		bool integer = host.integer;
		unsigned scale = host.scale;
		pthread_mutex_unlock(&host.lock);
		struct display *d = open_window(title, width, height, integer, scale);
		pthread_mutex_lock(&host.lock);
		host.d = d;
		host.closed = false;
		host.opened = true;
		pthread_cond_broadcast(&host.cond);
		pthread_mutex_unlock(&host.lock);
		if (d)
			run(d);
		pthread_mutex_lock(&host.lock);
	}
	host.running = false;
	pthread_mutex_unlock(&host.lock);
	pthread_join(thread, NULL);
	return b.ret;
}

struct display *display_open(const char *title, unsigned width, unsigned height, bool integer, unsigned scale)
{
	pthread_mutex_lock(&host.lock);
	if (!host.running) {
		pthread_mutex_unlock(&host.lock);
		snprintf(failure, sizeof failure, "no thread runs the window");
		return NULL;
	}
	host.title = title;
	host.width = width;
	host.height = height;
	host.integer = integer;
	host.scale = scale;
	host.opened = false;
	host.opening = true;
	pthread_cond_broadcast(&host.cond);
	while (!host.opened)
		pthread_cond_wait(&host.cond, &host.lock);
	struct display *d = host.d;
	pthread_mutex_unlock(&host.lock);
	return d;
}

void display_close(struct display *d)
{
	if (!d)
		return;
	pthread_mutex_lock(&host.lock);
	if (host.d == d) {
		d->in.close = true;
		wake(d);
		while (!host.closed)
			pthread_cond_wait(&host.cond, &host.lock);
	}
	pthread_mutex_unlock(&host.lock);
}

struct display_frame *display_frame_begin(struct display *d, unsigned width, unsigned height, unsigned bpp,
					  size_t row_bytes)
{
	pthread_mutex_lock(&host.lock);
	struct display_frame *f = &d->frame;
	f->kept = width == d->frame_w && height == d->frame_h && bpp == d->frame_bpp && row_bytes == d->frame_rb;
	if (!f->kept) {
		if (row_bytes * height > d->frame_bytes) {
			uint8_t *p = realloc(f->pixels, row_bytes * height);
			if (!p)
				goto fail;
			f->pixels = p;
			d->frame_bytes = row_bytes * height;
		}
		if (height > d->frame_rows) {
			uint8_t *p = realloc(f->changed, height);
			if (!p)
				goto fail;
			f->changed = p;
			d->frame_rows = height;
		}
		memset(f->changed, 0, height);
		d->frame_w = width;
		d->frame_h = height;
		d->frame_bpp = bpp;
		d->frame_rb = row_bytes;
	}
	return f;
fail:
	d->frame_w = d->frame_h = 0;
	pthread_mutex_unlock(&host.lock);
	return NULL;
}

void display_frame_end(struct display *d)
{
	d->frame_ended = true;
	d->frame_end_ns = clock_ns(CLOCK_MONOTONIC);
	pthread_mutex_unlock(&host.lock);
	wake(d);
}

void display_cursor(struct display *d, const struct display_cursor *c)
{
	struct display_cursor none = { .visible = false };
	if (!c || c->width > DISPLAY_CURSOR_MAX || c->height > DISPLAY_CURSOR_MAX)
		c = &none;
	pthread_mutex_lock(&host.lock);
	struct handed *in = &d->in;
	if (c->visible != in->cursor.visible || c->x != in->cursor.x || c->y != in->cursor.y ||
	    c->image != in->cursor.image) {
		if (c->image != in->cursor.image && c->pixels)
			memcpy(in->cursor_pixels, c->pixels, c->width * c->height * sizeof *c->pixels);
		in->cursor = *c;
		in->cursor.pixels = in->cursor_pixels;
		in->cursor_changed = true;
	}
	pthread_mutex_unlock(&host.lock);
}

bool display_wait(struct display *d, uint64_t ns)
{
	pthread_mutex_lock(&host.lock);
	if (!d->count && ns)
		input_wait(d, ns);
	bool any = d->count != 0;
	pthread_mutex_unlock(&host.lock);
	return any;
}

void display_capture_mouse(struct display *d, bool on)
{
	pthread_mutex_lock(&host.lock);
	d->in.capture = on;
	pthread_mutex_unlock(&host.lock);
	wake(d);
}

void display_menu(struct display *d, bool open)
{
	pthread_mutex_lock(&host.lock);
	d->in.menu = open;
	d->in.menu_changed = true;
	pthread_mutex_unlock(&host.lock);
	wake(d);
}

void display_menu_state(struct display *d, enum ui_mouse mouse, const char *status)
{
	pthread_mutex_lock(&host.lock);
	d->in.mouse = mouse;
	snprintf(d->in.status, sizeof d->in.status, "%s", status ? status : "");
	d->in.info_changed = true;
	pthread_mutex_unlock(&host.lock);
}

void display_tools_state(struct display *d, bool shown, const char *why_not)
{
	pthread_mutex_lock(&host.lock);
	d->in.tools = shown;
	snprintf(d->in.tools_why_not, sizeof d->in.tools_why_not, "%s", why_not ? why_not : "");
	d->in.info_changed = true;
	pthread_mutex_unlock(&host.lock);
}

void display_drives_state(struct display *d, const struct ui_drives *drives)
{
	pthread_mutex_lock(&host.lock);
	d->in.has_drives = drives != NULL;
	if (drives)
		d->in.drives = *drives;
	d->in.info_changed = true;
	pthread_mutex_unlock(&host.lock);
}

bool display_prompt(struct display *d, const char *text, const char *yes, const char *no)
{
	if (!d || (text && !d->ui))
		return false;
	pthread_mutex_lock(&host.lock);
	struct handed *in = &d->in;
	in->prompt = text != NULL;
	in->prompt_id++;
	snprintf(in->prompt_text, sizeof in->prompt_text, "%s", text ? text : "");
	snprintf(in->prompt_yes, sizeof in->prompt_yes, "%s", yes ? yes : "");
	snprintf(in->prompt_no, sizeof in->prompt_no, "%s", no ? no : "");
	in->prompt_alt[0] = '\0';
	in->prompt_changed = true;
	pthread_mutex_unlock(&host.lock);
	wake(d);
	return true;
}

void display_prompt_alt(struct display *d, const char *label)
{
	if (!d)
		return;
	pthread_mutex_lock(&host.lock);
	bool shown = d->in.prompt;
	if (shown) {
		snprintf(d->in.prompt_alt, sizeof d->in.prompt_alt, "%s", label ? label : "");
		d->in.prompt_changed = true;
	}
	pthread_mutex_unlock(&host.lock);
	if (shown)
		wake(d);
}

void display_follow(struct display *d, bool on)
{
	pthread_mutex_lock(&host.lock);
	d->in.follows = on;
	pthread_mutex_unlock(&host.lock);
}

void display_shortcuts(struct display *d, bool host_keys)
{
	pthread_mutex_lock(&host.lock);
	d->in.shortcuts = host_keys;
	pthread_mutex_unlock(&host.lock);
	wake(d);
}

void display_status_bar(struct display *d, bool on)
{
	pthread_mutex_lock(&host.lock);
	d->in.status_bar = on;
	pthread_mutex_unlock(&host.lock);
	wake(d);
}

void display_status(struct display *d, const struct ui_status *s)
{
	pthread_mutex_lock(&host.lock);
	/* (copied whole, padding too, so that comparing them whole works: the caller clears it) */
	if (memcmp(&d->in.bar, s, sizeof d->in.bar) != 0) {
		memcpy(&d->in.bar, s, sizeof d->in.bar);
		d->in.bar_changed = true;
	}
	pthread_mutex_unlock(&host.lock);
}

char *display_clipboard(struct display *d)
{
	if (!d->sdl.GetClipboardText || !d->sdl.free)
		return NULL;
	pthread_mutex_lock(&host.lock);
	d->in.clipboard_asked = true;
	d->in.clipboard_answered = false;
	pthread_mutex_unlock(&host.lock);
	wake(d);
	pthread_mutex_lock(&host.lock);
	while (!d->in.clipboard_answered && !host.closed)
		pthread_cond_wait(&host.cond, &host.lock);
	char *text = d->in.clipboard_text;
	d->in.clipboard_text = NULL;
	d->in.clipboard_answered = false;
	pthread_mutex_unlock(&host.lock);
	return text;
}

void display_set_clipboard(struct display *d, const char *text)
{
	char *copy = strdup(text);
	pthread_mutex_lock(&host.lock);
	free(d->in.clipboard);
	d->in.clipboard = copy;
	pthread_mutex_unlock(&host.lock);
	wake(d);
}

bool display_poll(struct display *d, struct display_event *ev)
{
	pthread_mutex_lock(&host.lock);
	for (;;) {
		if (!d->count) {
			pthread_mutex_unlock(&host.lock);
			return false;
		}
		struct queued q = d->queue[d->head];
		d->head = (d->head + 1) % QUEUE;
		d->count--;
		/* (an answer to a prompt that has gone since) */
		if (q.ev.type == DISPLAY_EVENT_MENU_ACTION && q.prompt && q.prompt != d->in.prompt_id)
			continue;
		pthread_mutex_unlock(&host.lock);
		*ev = q.ev;
		if (q.path) {
			snprintf(d->drop, sizeof d->drop, "%s", q.path);
			free(q.path);
			ev->path = d->drop;
		}
		return true;
	}
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
