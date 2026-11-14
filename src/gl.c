#include "gl.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Both platforms' code is built everywhere (it only uses what it loads), and
 * gl_open() tries CGL, then EGL: on each host the other one's library just
 * isn't there.
 */

struct platform {
	void (*close)(struct platform *p);
};

static void say(char *why, size_t size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(why, size, fmt, ap);
	va_end(ap);
}

bool gl_software_renderer(const char *renderer)
{
	static const char *const software[] = {
		"llvmpipe", "softpipe", "swrast", "Software Rasterizer", "Apple Software Renderer", "SwiftShader",
	};
	for (size_t k = 0; k < sizeof software / sizeof software[0]; k++)
		if (strstr(renderer, software[k]))
			return true;
	return false;
}

/* Loads GL_FUNCTIONS with `lookup` ("glEnable" and so on); false naming the first one missing */
static bool load_functions(struct gl *gl, void *(*lookup)(void *arg, const char *name), void *arg, char *why,
			   size_t why_size)
{
#define GL_LOAD(ret, name, params) \
	do { \
		*(void **)&gl->name = lookup(arg, "gl" #name); \
		if (!gl->name) { \
			say(why, why_size, "no gl" #name " in the OpenGL library"); \
			return false; \
		} \
	} while (0);
	GL_FUNCTIONS(GL_LOAD)
#undef GL_LOAD
	return true;
}

/* ---- macOS: CGL ------------------------------------------------------------------ */

typedef int CGLError;
typedef void *CGLPixelFormatObj;
typedef void *CGLContextObj;

enum { CGL_PFA_NO_RECOVERY = 72, CGL_PFA_ACCELERATED = 73, CGL_PFA_ALLOW_OFFLINE_RENDERERS = 96,
       CGL_PFA_OPENGL_PROFILE = 99, CGL_PFA_SUPPORTS_AUTOMATIC_GRAPHICS_SWITCHING = 101 };
#define CGL_OGLP_VERSION_3_2_CORE 0x3200       /* (gives the newest core profile: 4.1) */

struct cgl {
	struct platform base;
	void *lib;
	CGLPixelFormatObj pixel_format;
	CGLContextObj context;
	CGLError (*ChoosePixelFormat)(const int *attribs, CGLPixelFormatObj *pix, int *npix);
	CGLError (*DestroyPixelFormat)(CGLPixelFormatObj pix);
	CGLError (*CreateContext)(CGLPixelFormatObj pix, CGLContextObj share, CGLContextObj *ctx);
	CGLError (*DestroyContext)(CGLContextObj ctx);
	CGLError (*SetCurrentContext)(CGLContextObj ctx);
};

static void cgl_close(struct platform *p)
{
	struct cgl *c = (struct cgl *)p;
	if (c->context) {
		c->SetCurrentContext(NULL);
		c->DestroyContext(c->context);
	}
	if (c->pixel_format)
		c->DestroyPixelFormat(c->pixel_format);
	dlclose(c->lib);
	free(c);
}

static void *cgl_lookup(void *arg, const char *name)
{
	return dlsym(((struct cgl *)arg)->lib, name);
}

static bool cgl_open(struct gl *gl, bool allow_software, char *why, size_t why_size)
{
	void *lib = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		say(why, why_size, "no OpenGL framework");
		return false;
	}
	struct cgl *c = calloc(1, sizeof *c);
	if (!c) {
		dlclose(lib);
		say(why, why_size, "out of memory");
		return false;
	}
	c->base.close = cgl_close;
	c->lib = lib;
#define SYM(field, name) \
	do { \
		*(void **)&c->field = dlsym(lib, name); \
		if (!c->field) { \
			say(why, why_size, "no " name " in the OpenGL framework"); \
			goto fail; \
		} \
	} while (0)
	SYM(ChoosePixelFormat, "CGLChoosePixelFormat");
	SYM(DestroyPixelFormat, "CGLDestroyPixelFormat");
	SYM(CreateContext, "CGLCreateContext");
	SYM(DestroyContext, "CGLDestroyContext");
	SYM(SetCurrentContext, "CGLSetCurrentContext");
#undef SYM

	/*
	 * Hardware only (no recovery: no falling back to Apple's software renderer
	 * later), and the integrated GPU is fine: an off-screen context mustn't
	 * power up a discrete one.
	 */
	const int hardware[] = { CGL_PFA_OPENGL_PROFILE, CGL_OGLP_VERSION_3_2_CORE, CGL_PFA_ACCELERATED,
				 CGL_PFA_NO_RECOVERY, CGL_PFA_ALLOW_OFFLINE_RENDERERS,
				 CGL_PFA_SUPPORTS_AUTOMATIC_GRAPHICS_SWITCHING, 0 };
	const int any[] = { CGL_PFA_OPENGL_PROFILE, CGL_OGLP_VERSION_3_2_CORE, CGL_PFA_ALLOW_OFFLINE_RENDERERS,
			    CGL_PFA_SUPPORTS_AUTOMATIC_GRAPHICS_SWITCHING, 0 };
	int n = 0;
	if ((c->ChoosePixelFormat(hardware, &c->pixel_format, &n) || !c->pixel_format) && allow_software)
		c->ChoosePixelFormat(any, &c->pixel_format, &n);
	if (!c->pixel_format) {
		say(why, why_size, "no hardware OpenGL 3.2+ core pixel format");
		goto fail;
	}
	if (c->CreateContext(c->pixel_format, NULL, &c->context) || !c->context) {
		c->context = NULL;
		say(why, why_size, "CGLCreateContext failed");
		goto fail;
	}
	if (c->SetCurrentContext(c->context)) {
		say(why, why_size, "CGLSetCurrentContext failed");
		goto fail;
	}
	gl->priv = c;
	if (!load_functions(gl, cgl_lookup, c, why, why_size)) {
		gl->priv = NULL;
		goto fail;
	}
	return true;
fail:
	cgl_close(&c->base);
	return false;
}

/* ---- elsewhere: EGL ------------------------------------------------------------- */

/* The parts of the EGL 1.5 ABI and its extensions we use (Khronos's egl.h, eglext.h) */
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef void *EGLDeviceEXT;
typedef int32_t EGLint;
typedef unsigned EGLBoolean;
typedef unsigned EGLenum;

#define EGL_PBUFFER_BIT 0x0001
#define EGL_OPENGL_BIT 0x0008
#define EGL_SURFACE_TYPE 0x3033
#define EGL_NONE 0x3038
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_VENDOR 0x3053
#define EGL_EXTENSIONS 0x3055
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_OPENGL_API 0x30A2
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT 0x00000001
#define EGL_PLATFORM_DEVICE_EXT 0x313F           /* (EGL_EXT_platform_device) */
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD     /* (EGL_MESA_platform_surfaceless) */

struct egl {
	struct platform base;
	struct egl *next;               /* (in egl_users) */
	void *lib;
	void *gl_lib;                   /* libOpenGL or libGL, when eglGetProcAddress gives extensions only */
	bool all_procs;                 /* eglGetProcAddress gives core functions too */
	EGLDisplay display;
	EGLContext context;
	EGLSurface surface;
	EGLint (*GetError)(void);
	const char *(*QueryString)(EGLDisplay dpy, EGLint name);
	void (*(*GetProcAddress)(const char *name))(void);
	EGLDisplay (*GetDisplay)(void *native);
	EGLDisplay (*GetPlatformDisplay)(EGLenum platform, void *native, const intptr_t *attribs);
	EGLDisplay (*GetPlatformDisplayEXT)(EGLenum platform, void *native, const EGLint *attribs);
	EGLBoolean (*QueryDevicesEXT)(EGLint max, EGLDeviceEXT *devices, EGLint *n);
	EGLBoolean (*Initialize)(EGLDisplay dpy, EGLint *major, EGLint *minor);
	EGLBoolean (*Terminate)(EGLDisplay dpy);
	EGLBoolean (*BindAPI)(EGLenum api);
	EGLBoolean (*ChooseConfig)(EGLDisplay dpy, const EGLint *attribs, EGLConfig *configs, EGLint size, EGLint *n);
	EGLContext (*CreateContext)(EGLDisplay dpy, EGLConfig config, EGLContext share, const EGLint *attribs);
	EGLBoolean (*DestroyContext)(EGLDisplay dpy, EGLContext ctx);
	EGLSurface (*CreatePbufferSurface)(EGLDisplay dpy, EGLConfig config, const EGLint *attribs);
	EGLBoolean (*DestroySurface)(EGLDisplay dpy, EGLSurface surface);
	EGLBoolean (*MakeCurrent)(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
	EGLBoolean (*ReleaseThread)(void);
};

/* A display to try: a platform's (0: eglGetDisplay(EGL_DEFAULT_DISPLAY)) */
struct egl_display {
	EGLenum platform;
	void *native;
};

/* Whether a space-separated list (or NULL) has a name */
static bool has_extension(const char *list, const char *name)
{
	size_t n = strlen(name);
	for (const char *p = list; p && (p = strstr(p, name)); p += n)
		if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0'))
			return true;
	return false;
}

static void *egl_proc(struct egl *e, const char *name)
{
	void (*f)(void) = e->GetProcAddress(name);
	void *p;
	memcpy(&p, &f, sizeof p);
	return p;
}

static void *egl_lookup(void *arg, const char *name)
{
	struct egl *e = arg;
	return e->all_procs ? egl_proc(e, name) : dlsym(e->gl_lib, name);
}

/*
 * A platform's display is the whole process's, and eglTerminate ends it for
 * every thread: it's terminated when the last of these stops using it.
 */
static pthread_mutex_t egl_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct egl *egl_users;           /* (the ones with a display) */

static bool egl_initialize(struct egl *e, EGLDisplay display, EGLint *major, EGLint *minor)
{
	pthread_mutex_lock(&egl_mutex);
	bool ok = e->Initialize(display, major, minor);
	if (ok) {
		e->display = display;
		e->next = egl_users;
		egl_users = e;
	} else {
		e->Terminate(display);
	}
	pthread_mutex_unlock(&egl_mutex);
	return ok;
}

/* Ends the display being tried and what was made on it */
static void egl_end(struct egl *e)
{
	if (!e->display)
		return;
	if (e->context) {
		e->MakeCurrent(e->display, NULL, NULL, NULL);
		e->DestroyContext(e->display, e->context);
	}
	if (e->surface)
		e->DestroySurface(e->display, e->surface);
	pthread_mutex_lock(&egl_mutex);
	bool shared = false;
	for (struct egl **p = &egl_users; *p;) {
		if (*p == e) {
			*p = e->next;
			continue;
		}
		shared |= (*p)->display == e->display;
		p = &(*p)->next;
	}
	if (!shared)
		e->Terminate(e->display);
	pthread_mutex_unlock(&egl_mutex);
	e->display = e->context = e->surface = NULL;
}

static void egl_close(struct platform *p)
{
	struct egl *e = (struct egl *)p;
	egl_end(e);
	if (e->ReleaseThread)
		e->ReleaseThread();
	if (e->gl_lib)
		dlclose(e->gl_lib);
	dlclose(e->lib);
	free(e);
}

static EGLDisplay egl_get_display(struct egl *e, const struct egl_display *d)
{
	if (!d->platform)
		return e->GetDisplay(NULL);
	if (e->GetPlatformDisplayEXT)
		return e->GetPlatformDisplayEXT(d->platform, d->native, NULL);
	return e->GetPlatformDisplay(d->platform, d->native, NULL);
}

/*
 * Makes an OpenGL 3.3 core context on a display current and loads the
 * functions. Returns how far it got (0: no display, 1: a display, 2: a
 * context, 3: all of it), with the reason it stopped.
 */
static int egl_try(struct egl *e, struct gl *gl, const struct egl_display *d, const char *client, char *why,
		   size_t why_size)
{
	EGLDisplay display = egl_get_display(e, d);
	EGLint major = 0, minor = 0;
	if (!display || !egl_initialize(e, display, &major, &minor)) {
		say(why, why_size, "no EGL display without a window system");
		return 0;
	}
	const char *vendor = e->QueryString(display, EGL_VENDOR);
	const char *extensions = e->QueryString(display, EGL_EXTENSIONS);
	vendor = vendor ? vendor : "?";
	bool surfaceless = has_extension(extensions, "EGL_KHR_surfaceless_context");

	const EGLint config_attribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE,
					  surfaceless ? 0 : EGL_PBUFFER_BIT, EGL_NONE };
	const EGLint context_attribs[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
					   EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
					   EGL_NONE };
	const EGLint pbuffer_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
	EGLConfig config = NULL;
	EGLint n = 0;
	if (!e->BindAPI(EGL_OPENGL_API)) {
		say(why, why_size, "EGL (%s) has no desktop OpenGL", vendor);
		return 1;
	}
	if (!e->ChooseConfig(display, config_attribs, &config, 1, &n) || n < 1) {
		say(why, why_size, "EGL (%s) has no OpenGL config", vendor);
		return 1;
	}
	e->context = e->CreateContext(display, config, NULL, context_attribs);
	if (!e->context) {
		say(why, why_size, "EGL (%s) has no OpenGL 3.3 core context (error 0x%x)", vendor,
		    (unsigned)e->GetError());
		return 1;
	}
	if (!surfaceless && !(e->surface = e->CreatePbufferSurface(display, config, pbuffer_attribs))) {
		say(why, why_size, "EGL (%s) has no pbuffer (error 0x%x)", vendor, (unsigned)e->GetError());
		return 2;
	}
	if (!e->MakeCurrent(display, e->surface, e->surface, e->context)) {
		say(why, why_size, "EGL (%s) can't make an OpenGL context current (error 0x%x)", vendor,
		    (unsigned)e->GetError());
		return 2;
	}

	e->all_procs = major > 1 || (major == 1 && minor >= 5) ||
		       has_extension(extensions, "EGL_KHR_get_all_proc_addresses") ||
		       has_extension(client, "EGL_KHR_client_get_all_proc_addresses");
	if (!e->all_procs && !e->gl_lib) {
		e->gl_lib = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_LOCAL);
		if (!e->gl_lib)
			e->gl_lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
		if (!e->gl_lib) {
			say(why, why_size, "no libOpenGL.so.0 or libGL.so.1");
			return 2;
		}
	}
	return load_functions(gl, egl_lookup, e, why, why_size) ? 3 : 2;
}

static bool egl_open(struct gl *gl, bool allow_software, char *why, size_t why_size)
{
	void *lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		say(why, why_size, "no libEGL.so.1");
		return false;
	}
	struct egl *e = calloc(1, sizeof *e);
	if (!e) {
		dlclose(lib);
		say(why, why_size, "out of memory");
		return false;
	}
	e->base.close = egl_close;
	e->lib = lib;
#define SYM(field, name) \
	do { \
		*(void **)&e->field = dlsym(lib, name); \
		if (!e->field) { \
			say(why, why_size, "no " name " in libEGL.so.1"); \
			goto fail; \
		} \
	} while (0)
	SYM(GetError, "eglGetError");
	SYM(QueryString, "eglQueryString");
	SYM(GetProcAddress, "eglGetProcAddress");
	SYM(GetDisplay, "eglGetDisplay");
	SYM(Initialize, "eglInitialize");
	SYM(Terminate, "eglTerminate");
	SYM(BindAPI, "eglBindAPI");
	SYM(ChooseConfig, "eglChooseConfig");
	SYM(CreateContext, "eglCreateContext");
	SYM(DestroyContext, "eglDestroyContext");
	SYM(CreatePbufferSurface, "eglCreatePbufferSurface");
	SYM(DestroySurface, "eglDestroySurface");
	SYM(MakeCurrent, "eglMakeCurrent");
	SYM(ReleaseThread, "eglReleaseThread");
#undef SYM

	/*
	 * Displays with no window system: Mesa's surfaceless one, each device
	 * (NVIDIA's driver has those), then the default one. The client
	 * extensions (NULL without EGL 1.5 or EGL_EXT_client_extensions) say
	 * which platforms there are.
	 */
	const char *client = e->QueryString(NULL, EGL_EXTENSIONS);
	*(void **)&e->GetPlatformDisplay = dlsym(lib, "eglGetPlatformDisplay");
	if (has_extension(client, "EGL_EXT_platform_base"))
		*(void **)&e->GetPlatformDisplayEXT = egl_proc(e, "eglGetPlatformDisplayEXT");
	bool platforms = e->GetPlatformDisplay || e->GetPlatformDisplayEXT;
	if (platforms && has_extension(client, "EGL_EXT_platform_device") &&
	    (has_extension(client, "EGL_EXT_device_enumeration") || has_extension(client, "EGL_EXT_device_base")))
		*(void **)&e->QueryDevicesEXT = egl_proc(e, "eglQueryDevicesEXT");
	enum { MAX_DEVICES = 8 };
	struct egl_display displays[MAX_DEVICES + 2];
	int n = 0;
	if (platforms && has_extension(client, "EGL_MESA_platform_surfaceless"))
		displays[n++] = (struct egl_display){ EGL_PLATFORM_SURFACELESS_MESA, NULL };
	EGLDeviceEXT devices[MAX_DEVICES];
	EGLint devices_n = 0;
	if (e->QueryDevicesEXT && e->QueryDevicesEXT(MAX_DEVICES, devices, &devices_n))
		for (EGLint k = 0; k < devices_n && k < MAX_DEVICES; k++)
			displays[n++] = (struct egl_display){ EGL_PLATFORM_DEVICE_EXT, devices[k] };
	displays[n++] = (struct egl_display){ 0, NULL };

	/* The first hardware renderer; else, if allowed, the first software one */
	int reached = -1, software = -1;
	for (int k = 0; k < n; k++) {
		char reason[160];
		int got = egl_try(e, gl, &displays[k], client, reason, sizeof reason);
		if (got == 3) {
			const char *renderer = (const char *)gl->GetString(GL_RENDERER);
			if (!renderer || !gl_software_renderer(renderer))
				goto done;
			say(reason, sizeof reason, "OpenGL renderer %s is software, not hardware", renderer);
			if (software < 0)
				software = k;
		}
		egl_end(e);
		if (got > reached) {
			reached = got;
			say(why, why_size, "%s", reason);
		}
	}
	if (allow_software && software >= 0 && egl_try(e, gl, &displays[software], client, why, why_size) == 3)
		goto done;
fail:
	egl_close(&e->base);
	return false;
done:
	gl->priv = e;
	return true;
}

/* ---- both --------------------------------------------------------------------------- */

bool gl_open(struct gl *gl, bool allow_software, char *why, size_t why_size)
{
	char cgl_why[160], egl_why[160];
	memset(gl, 0, sizeof *gl);
	if (!cgl_open(gl, allow_software, cgl_why, sizeof cgl_why) &&
	    !egl_open(gl, allow_software, egl_why, sizeof egl_why)) {
		/* (the reason from the platform this host has) */
		say(why, why_size, "%s", strstr(cgl_why, "no OpenGL framework") ? egl_why : cgl_why);
		memset(gl, 0, sizeof *gl);
		return false;
	}
	while (gl->GetError() != GL_NO_ERROR)
		;
	const char *renderer = (const char *)gl->GetString(GL_RENDERER);
	const char *version = (const char *)gl->GetString(GL_VERSION);
	snprintf(gl->renderer, sizeof gl->renderer, "%s", renderer ? renderer : "?");
	snprintf(gl->version, sizeof gl->version, "%s", version ? version : "?");
	gl->GetIntegerv(GL_MAJOR_VERSION, &gl->major);
	gl->GetIntegerv(GL_MINOR_VERSION, &gl->minor);
	GLint profile = 0;
	gl->GetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
	if (gl->major < 3 || (gl->major == 3 && gl->minor < 3) || !(profile & GL_CONTEXT_CORE_PROFILE_BIT)) {
		say(why, why_size, "OpenGL %s on %s is older than 3.3 core", gl->version, gl->renderer);
		gl_close(gl);
		return false;
	}
	if (!allow_software && gl_software_renderer(gl->renderer)) {
		say(why, why_size, "OpenGL renderer %s is software, not hardware", gl->renderer);
		gl_close(gl);
		return false;
	}
	return true;
}

void gl_close(struct gl *gl)
{
	struct platform *p = gl->priv;
	if (p)
		p->close(p);
	memset(gl, 0, sizeof *gl);
}
