#include "gpu.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl.h"

/* ---- shaders ------------------------------------------------------------------------- */

/*
 * Everything is drawn with w = 1, so the GPU interpolates linearly in screen
 * space, as raster.c does; texture coordinates and fog divide by 1/w per
 * pixel. Rows go upward in GL, so image row y is GL row y: drawn upside
 * down, read back the right way up.
 */
static const char vertex_source[] =
	"layout(location = 0) in vec4 a_pos;\n"          /* x, y (pixels), z, 1/w */
	"layout(location = 1) in vec4 a_color;\n"
	"layout(location = 2) in vec2 a_uvw;\n"          /* u/w, v/w (bitmaps: texel coordinates) */
	"layout(location = 3) in vec3 a_kd;\n"
	"layout(location = 4) in vec3 a_ks;\n"
	"uniform vec2 u_size;\n"
	"noperspective out float v_invw;\n"
	"noperspective out vec4 v_color;\n"
	"noperspective out vec2 v_uvw;\n"
	"noperspective out vec3 v_kd;\n"
	"noperspective out vec3 v_ks;\n"
	"void main()\n"
	"{\n"
	"	gl_Position = vec4(a_pos.x * 2.0 / u_size.x - 1.0, a_pos.y * 2.0 / u_size.y - 1.0,\n"
	"			   a_pos.z * 2.0 - 1.0, 1.0);\n"
	"	v_invw = a_pos.w;\n"
	"	v_color = a_color;\n"
	"	v_uvw = a_uvw;\n"
	"	v_kd = a_kd;\n"
	"	v_ks = a_ks;\n"
	"}\n";

/* raster.c's fragment(), step for step */
static const char fragment_source[] =
	"noperspective in float v_invw;\n"
	"noperspective in vec4 v_color;\n"
	"noperspective in vec2 v_uvw;\n"
	"noperspective in vec3 v_kd;\n"
	"noperspective in vec3 v_ks;\n"
	"uniform sampler2D u_texture;\n"
	"uniform sampler2D u_texture2;\n"
	"uniform bool u_second;\n"                       /* (the second texture modulates, at ks) */
	"uniform int u_mode;\n"                          /* 0: color, 1: textured, 2: bitmap */
	"uniform int u_texture_op;\n"
	"uniform int u_fog_mode;\n"
	"uniform vec3 u_fog_color;\n"
	"uniform float u_fog_start, u_fog_end, u_fog_density;\n"
	"uniform int u_alpha_test;\n"
	"uniform float u_alpha_ref;\n"
	"uniform bool u_interpolate;\n"
	"layout(location = 0) out vec4 o_color;\n"
	"void main()\n"
	"{\n"
	"#ifdef PERSPECTIVE_Z\n"
	"	gl_FragDepth = v_invw > 0.0 ? v_invw / (1.0 + v_invw) : 0.0;\n"
	"#endif\n"
	"	float w = v_invw != 0.0 ? 1.0 / v_invw : 1e30;\n"
	"	vec4 c = v_color;\n"
	"	if (u_mode == 1) {\n"
	"		vec4 t = texture(u_texture, v_uvw * w);\n"
	"		if ((u_texture_op & 4) != 0)\n"
	"			c.rgb = t.a * t.rgb + (1.0 - t.a) * v_color.rgb;\n"
	"		else\n"
	"			c = vec4(t.rgb, t.a * v_color.a);\n"
	"		if ((u_texture_op & 1) != 0)\n"
	"			c.rgb = clamp(c.rgb * v_kd, 0.0, 1.0);\n"
	"		if (u_second)\n"
	"			c *= texture(u_texture2, v_ks.xy * (v_ks.z != 0.0 ? 1.0 / v_ks.z : 1e30));\n"
	"		else if ((u_texture_op & 2) != 0)\n"
	"			c.rgb = clamp(c.rgb + v_ks, 0.0, 1.0);\n"
	"	} else if (u_mode == 2) {\n"
	"		c = texelFetch(u_texture, ivec2(floor(v_uvw)), 0);\n"
	"		if (c.a <= 0.0)\n"
	"			discard;\n"
	"	}\n"
	"	if (u_fog_mode != 0) {\n"
	"		float f, d = u_fog_end != 0.0 ? u_fog_density / u_fog_end : u_fog_density;\n"
	"		if (u_fog_mode == 1) {\n"
	"			f = c.a;\n"
	"			c.a = 1.0;\n"
	"		} else if (u_fog_mode == 2) {\n"
	"			f = w < u_fog_start ? 1.0 : w > u_fog_end ? 0.0 : (u_fog_end - w) / (u_fog_end - u_fog_start);\n"
	"		} else if (u_fog_mode == 3) {\n"
	"			f = exp(-d * w);\n"
	"		} else {\n"
	"			f = exp(-(d * w) * (d * w));\n"
	"		}\n"
	"		f = clamp(f, 0.0, 1.0);\n"
	"		c.rgb = f * c.rgb + (1.0 - f) * u_fog_color;\n"
	"	}\n"
	"	if (u_alpha_test != 0) {\n"
	"		bool pass = u_alpha_test == 1 ? c.a < u_alpha_ref : u_alpha_test == 2 ? c.a == u_alpha_ref :\n"
	"			    u_alpha_test == 3 ? c.a <= u_alpha_ref : u_alpha_test == 4 ? c.a > u_alpha_ref :\n"
	"			    u_alpha_test == 5 ? c.a != u_alpha_ref : u_alpha_test == 6 ? c.a >= u_alpha_ref : true;\n"
	"		if (!pass)\n"
	"			discard;\n"
	"	}\n"
	"	if (c.a <= 0.0 && (u_interpolate || all(lessThanEqual(c.rgb, vec3(0.0)))))\n"
	"		discard;\n"
	"	o_color = c;\n"
	"}\n";

enum { MODE_COLOR, MODE_TEXTURED, MODE_BITMAP };
enum { PROGRAM_Z, PROGRAM_PERSPECTIVE_Z, PROGRAMS };

struct program {
	GLuint id;
	GLint size, texture, texture2, second, mode, texture_op, fog_mode, fog_color, fog_start, fog_end, fog_density,
	      alpha_test, alpha_ref, interpolate;
};

#define FLOATS 16                       /* per vertex: x, y, z, 1/w, r, g, b, a, u/w, v/w, kd[3], ks[3] */

/* ---- the backend ----------------------------------------------------------------- */

struct gpu_target {
	int width, height;
	GLuint fbo, color, depth;
};

enum job { JOB_NONE, JOB_TARGET_NEW, JOB_TARGET_FREE, JOB_COPY, JOB_DRAW, JOB_READ, JOB_QUIT };

/* Commands cut into draw calls: consecutive ones that draw the same way */
struct range {
	unsigned first, count;          /* vertices (a clear: none) */
	const struct raster_cmd *clear;
	unsigned state;
	int mode;
	const struct raster_texture *texture;
};

struct gpu {
	pthread_t thread;
	pthread_mutex_t mu;
	pthread_cond_t posted, done;
	bool allow_software;
	bool init_done, ready;          /* (set once, by the thread) */
	bool failed;
	char why[256];
	char renderer[300];
	/* the job in hand (one at a time) */
	bool busy;
	enum job job;
	struct gpu_target *target, *from;
	const struct raster_state *states;
	const struct raster_cmd *cmds;
	unsigned n;
	uint32_t *color;
	bool job_ok;
	/* GL textures to delete (from gpu_texture_free()) */
	GLuint *dead;
	unsigned ndead, dead_cap;

	/* the thread's */
	struct gl gl;
	struct program program[PROGRAMS];
	GLuint vao, vbo;
	GLuint sampler[12], bitmap_sampler;     /* sampler[filter * 4 + clamp_u * 2 + clamp_v] */
	GLuint blank;                   /* (bound when drawing without a texture: drivers want one) */
	float anisotropy;
	float *verts;
	size_t nverts, verts_cap;       /* (vertices) */
	struct range *ranges;
	unsigned nranges, ranges_cap;
	bool debug;
	unsigned long jobs, fail_at;
};

static void debug(struct gpu *g, const char *fmt, ...)
{
	if (!g->debug)
		return;
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "gpu: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* ---- the thread: programs, samplers, textures ---------------------------------------- */

static GLuint compile(struct gpu *g, GLenum type, const char *defines, const char *body)
{
	struct gl *gl = &g->gl;
	GLuint s = gl->CreateShader(type);
	const GLchar *src[3] = { "#version 330 core\n", defines, body };
	gl->ShaderSource(s, 3, src, NULL);
	gl->CompileShader(s);
	GLint ok = 0;
	gl->GetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok || g->debug) {
		char log[2048] = "";
		gl->GetShaderInfoLog(s, sizeof log, NULL, log);
		if (!ok || log[0])
			fprintf(stderr, "gpu: %s shader: %s\n", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
	}
	if (!ok) {
		gl->DeleteShader(s);
		return 0;
	}
	return s;
}

static bool make_program(struct gpu *g, struct program *p, const char *defines)
{
	struct gl *gl = &g->gl;
	GLuint vs = compile(g, GL_VERTEX_SHADER, "", vertex_source);
	GLuint fs = compile(g, GL_FRAGMENT_SHADER, defines, fragment_source);
	if (!vs || !fs)
		return false;
	p->id = gl->CreateProgram();
	gl->AttachShader(p->id, vs);
	gl->AttachShader(p->id, fs);
	gl->LinkProgram(p->id);
	gl->DeleteShader(vs);
	gl->DeleteShader(fs);
	GLint ok = 0;
	gl->GetProgramiv(p->id, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[2048] = "";
		gl->GetProgramInfoLog(p->id, sizeof log, NULL, log);
		fprintf(stderr, "gpu: program: %s\n", log);
		return false;
	}
#define U(field, name) p->field = gl->GetUniformLocation(p->id, name)
	U(size, "u_size");
	U(texture, "u_texture");
	U(texture2, "u_texture2");
	U(second, "u_second");
	U(mode, "u_mode");
	U(texture_op, "u_texture_op");
	U(fog_mode, "u_fog_mode");
	U(fog_color, "u_fog_color");
	U(fog_start, "u_fog_start");
	U(fog_end, "u_fog_end");
	U(fog_density, "u_fog_density");
	U(alpha_test, "u_alpha_test");
	U(alpha_ref, "u_alpha_ref");
	U(interpolate, "u_interpolate");
#undef U
	gl->UseProgram(p->id);
	gl->Uniform1i(p->texture, 0);
	gl->Uniform1i(p->texture2, 1);
	return true;
}

static bool has_extension(struct gpu *g, const char *name)
{
	GLint n = 0;
	g->gl.GetIntegerv(GL_NUM_EXTENSIONS, &n);
	for (GLint k = 0; k < n; k++) {
		const char *e = (const char *)g->gl.GetStringi(GL_EXTENSIONS, (GLuint)k);
		if (e && strcmp(e, name) == 0)
			return true;
	}
	return false;
}

/*
 * RAVE's texture filters: Fast is nearest, from the nearest mipmap level;
 * Mid bilinear, from the nearest level; Best trilinear (and anisotropic,
 * where there's some). The GPU picks the levels per pixel.
 */
static void make_samplers(struct gpu *g)
{
	struct gl *gl = &g->gl;
	static const GLint min[3] = { GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_LINEAR };
	static const GLint mag[3] = { GL_NEAREST, GL_LINEAR, GL_LINEAR };
	gl->GenSamplers(12, g->sampler);
	for (unsigned k = 0; k < 12; k++) {
		GLuint s = g->sampler[k];
		unsigned filter = k / 4;
		gl->SamplerParameteri(s, GL_TEXTURE_MIN_FILTER, min[filter]);
		gl->SamplerParameteri(s, GL_TEXTURE_MAG_FILTER, mag[filter]);
		gl->SamplerParameteri(s, GL_TEXTURE_WRAP_S, k & 2 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
		gl->SamplerParameteri(s, GL_TEXTURE_WRAP_T, k & 1 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
		if (filter == 2 && g->anisotropy > 1)
			gl->SamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY, g->anisotropy);
	}
	gl->GenSamplers(1, &g->bitmap_sampler);
	gl->SamplerParameteri(g->bitmap_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl->SamplerParameteri(g->bitmap_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

/* A texture's GL copy, made the first time it's drawn with (its texels never change) */
static GLuint texture_of(struct gpu *g, const struct raster_texture *ct)
{
	struct raster_texture *t = (struct raster_texture *)ct;
	if (t->gpu)
		return t->gpu;
	struct gl *gl = &g->gl;
	gl->GenTextures(1, &t->gpu);
	gl->BindTexture(GL_TEXTURE_2D, t->gpu);
	/* (a level not half the one before would leave the texture incomplete: those from it go) */
	unsigned levels = 1;
	while (levels < t->levels && t->width[levels] == (t->width[levels - 1] > 1 ? t->width[levels - 1] / 2 : 1) &&
	       t->height[levels] == (t->height[levels - 1] > 1 ? t->height[levels - 1] / 2 : 1))
		levels++;
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
	for (unsigned l = 0; l < levels; l++)
		gl->TexImage2D(GL_TEXTURE_2D, (GLint)l, GL_RGBA8, (GLsizei)t->width[l], (GLsizei)t->height[l], 0, GL_BGRA,
			       GL_UNSIGNED_INT_8_8_8_8_REV, t->texels[l]);
	return t->gpu;
}

static void delete_dead(struct gpu *g)
{
	pthread_mutex_lock(&g->mu);
	if (g->ndead)
		g->gl.DeleteTextures((GLsizei)g->ndead, g->dead);
	g->ndead = 0;
	pthread_mutex_unlock(&g->mu);
}

/* ---- the thread: drawing -------------------------------------------------------------- */

static bool new_target(struct gpu *g, struct gpu_target *t)
{
	struct gl *gl = &g->gl;
	gl->GenRenderbuffers(1, &t->color);
	gl->BindRenderbuffer(GL_RENDERBUFFER, t->color);
	gl->RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, t->width, t->height);
	gl->GenRenderbuffers(1, &t->depth);
	gl->BindRenderbuffer(GL_RENDERBUFFER, t->depth);
	gl->RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, t->width, t->height);
	gl->GenFramebuffers(1, &t->fbo);
	gl->BindFramebuffer(GL_FRAMEBUFFER, t->fbo);
	gl->FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t->color);
	gl->FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth);
	if (gl->CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return false;
	gl->Disable(GL_SCISSOR_TEST);
	gl->ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	gl->DepthMask(GL_TRUE);
	gl->ClearColor(0, 0, 0, 0);
	gl->ClearDepth(0);
	gl->Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	gl->Enable(GL_SCISSOR_TEST);
	return true;
}

static void free_target(struct gpu *g, struct gpu_target *t)
{
	struct gl *gl = &g->gl;
	gl->DeleteFramebuffers(1, &t->fbo);
	gl->DeleteRenderbuffers(1, &t->color);
	gl->DeleteRenderbuffers(1, &t->depth);
	free(t);
}

/* A raster.c depth value in the GL depth buffer (see the fragment shader) */
static double gl_depth(const struct raster_state *s, float z)
{
	if (!s->perspective_z)
		return z < 0 ? 0 : z > 1 ? 1 : z;
	double invw = -(double)z;
	return invw > 0 ? invw / (1 + invw) : 0;
}

static GLenum depth_func(const struct raster_state *s)
{
	static const GLenum direct[] = { GL_ALWAYS, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL,
					 GL_ALWAYS, GL_NEVER };
	/* (perspective z is tested as -1/w, stored as an increasing function of 1/w) */
	static const GLenum flipped[] = { GL_ALWAYS, GL_GREATER, GL_EQUAL, GL_GEQUAL, GL_LESS, GL_NOTEQUAL, GL_LEQUAL,
					  GL_ALWAYS, GL_NEVER };
	unsigned f = s->z_func <= RASTER_Z_FALSE ? s->z_func : RASTER_Z_TRUE;
	return s->perspective_z ? flipped[f] : direct[f];
}

static GLenum blend_factor(unsigned f)
{
	static const GLenum gl[] = { GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA,
				     GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR,
				     GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE };
	return f < sizeof gl / sizeof gl[0] ? gl[f] : GL_ONE;
}

static float *new_vertices(struct gpu *g, size_t n)
{
	if (g->nverts + n > g->verts_cap) {
		size_t cap = g->verts_cap ? 2 * g->verts_cap : 4096;
		while (cap < g->nverts + n)
			cap *= 2;
		float *v = realloc(g->verts, cap * FLOATS * sizeof *v);
		if (!v)
			return NULL;
		g->verts = v;
		g->verts_cap = cap;
	}
	float *v = g->verts + g->nverts * FLOATS;
	g->nverts += n;
	return v;
}

static void put_vertex(float *p, const struct raster_vertex *v, float x, float y)
{
	const float f[FLOATS] = { x, y, v->z, v->invw, v->r, v->g, v->b, v->a, v->uw, v->vw,
				  v->kd[0], v->kd[1], v->kd[2], v->ks[0], v->ks[1], v->ks[2] };
	memcpy(p, f, sizeof f);
}

/* A rectangle of pixels [x0, x1) x [y0, y1) with one vertex's values, as two triangles */
static bool put_rect(struct gpu *g, const struct raster_vertex *v, float x0, float y0, float x1, float y1)
{
	float *p = new_vertices(g, 6);
	if (!p)
		return false;
	const float corner[6][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y0 }, { x1, y1 }, { x0, y1 } };
	for (int k = 0; k < 6; k++)
		put_vertex(p + k * FLOATS, v, corner[k][0], corner[k][1]);
	return true;
}

/* A square around a point, where raster.c's plot() puts it */
static bool put_square(struct gpu *g, const struct raster_state *s, const struct raster_vertex *v, float cx, float cy)
{
	float half = (s->width > 1 ? s->width : 1) * 0.5f;
	return put_rect(g, v, floorf(cx - half + 0.5f), floorf(cy - half + 0.5f), floorf(cx + half + 0.5f),
			floorf(cy + half + 0.5f));
}

/* A line as raster.c draws it: a square at each step, its values interpolated (the last pixel left out) */
static bool put_line(struct gpu *g, const struct raster_state *s, const struct raster_vertex *v0,
		     const struct raster_vertex *v1)
{
	float dx = v1->x - v0->x, dy = v1->y - v0->y;
	float len = fmaxf(fabsf(dx), fabsf(dy));
	if (!(len < 1e5f))
		return true;
	int steps = (int)ceilf(len);
	for (int k = 0; k < steps; k++) {
		float f = (float)k / (float)steps;
		struct raster_vertex v = *v0;
		v.z = v0->z + (v1->z - v0->z) * f;
		v.invw = v0->invw + (v1->invw - v0->invw) * f;
		v.r = v0->r + (v1->r - v0->r) * f;
		v.g = v0->g + (v1->g - v0->g) * f;
		v.b = v0->b + (v1->b - v0->b) * f;
		v.a = v0->a + (v1->a - v0->a) * f;
		if (!put_square(g, s, &v, v0->x + dx * f, v0->y + dy * f))
			return false;
	}
	return true;
}

/* An image unscaled at (x, y), with texel coordinates to fetch by */
static bool put_bitmap(struct gpu *g, const struct raster_texture *image, const struct raster_cmd *c)
{
	float x0 = floorf(c->v[0].x + 0.5f), y0 = floorf(c->v[0].y + 0.5f);
	float w = (float)image->width[0], h = (float)image->height[0];
	struct raster_vertex v;
	memset(&v, 0, sizeof v);
	v.z = c->v[0].z;
	v.invw = 1;
	float *p = new_vertices(g, 6);
	if (!p)
		return false;
	const float corner[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
	for (int k = 0; k < 6; k++) {
		v.uw = corner[k][0] * w;
		v.vw = corner[k][1] * h;
		put_vertex(p + k * FLOATS, &v, x0 + v.uw, y0 + v.vw);
	}
	return true;
}

static bool add_range(struct gpu *g, unsigned first, const struct raster_cmd *clear, unsigned state, int mode,
		      const struct raster_texture *texture)
{
	unsigned count = (unsigned)g->nverts - first;
	if (!clear && !count)
		return true;
	struct range *last = g->nranges ? &g->ranges[g->nranges - 1] : NULL;
	if (!clear && last && !last->clear && last->state == state && last->mode == mode && last->texture == texture &&
	    last->first + last->count == first) {
		last->count += count;
		return true;
	}
	if (g->nranges == g->ranges_cap) {
		unsigned cap = g->ranges_cap ? 2 * g->ranges_cap : 256;
		struct range *r = realloc(g->ranges, cap * sizeof *r);
		if (!r)
			return false;
		g->ranges = r;
		g->ranges_cap = cap;
	}
	g->ranges[g->nranges++] = (struct range){ first, count, clear, state, mode, texture };
	return true;
}

/* The commands' vertices, and the draw calls (and clears) they make, in order */
static bool build(struct gpu *g, const struct raster_state *states, const struct raster_cmd *cmds, unsigned n)
{
	g->nverts = 0;
	g->nranges = 0;
	for (unsigned k = 0; k < n; k++) {
		const struct raster_cmd *c = &cmds[k];
		const struct raster_state *s = &states[c->state];
		unsigned first = (unsigned)g->nverts;
		int mode = MODE_COLOR;
		const struct raster_texture *texture = NULL;
		bool ok = true;
		switch (c->kind) {
		case RASTER_CMD_TRIANGLE: {
			if (c->textured && s->texture && s->texture->levels) {
				mode = MODE_TEXTURED;
				texture = s->texture;
			}
			float *p = new_vertices(g, 3);
			ok = p != NULL;
			for (int j = 0; ok && j < 3; j++)
				put_vertex(p + j * FLOATS, &c->v[j], c->v[j].x, c->v[j].y);
			break;
		}
		case RASTER_CMD_LINE:
			ok = put_line(g, s, &c->v[0], &c->v[1]);
			break;
		case RASTER_CMD_POINT:
			ok = put_square(g, s, &c->v[0], c->v[0].x, c->v[0].y);
			break;
		case RASTER_CMD_BITMAP:
			if (!c->image || !c->image->levels)
				continue;
			mode = MODE_BITMAP;
			texture = c->image;
			ok = put_bitmap(g, c->image, c);
			break;
		case RASTER_CMD_CLEAR:
			ok = add_range(g, first, c, c->state, 0, NULL);
			continue;
		default:
			continue;
		}
		if (!ok || !add_range(g, first, NULL, c->state, mode, texture))
			return false;
	}
	return true;
}

static void scissor(struct gpu *g, const struct gpu_target *t, int l, int top, int r, int b)
{
	l = l > 0 ? l : 0;
	top = top > 0 ? top : 0;
	r = r < t->width ? r : t->width;
	b = b < t->height ? b : t->height;
	if (r < l)
		r = l;
	if (b < top)
		b = top;
	g->gl.Scissor(l, top, r - l, b - top);
}

static void clear(struct gpu *g, const struct gpu_target *t, const struct raster_state *s, const struct raster_cmd *c)
{
	struct gl *gl = &g->gl;
	/* (as raster.c: the rectangle's rows within the clip's) */
	scissor(g, t, c->rect[0], c->rect[1] > s->clip_top ? c->rect[1] : s->clip_top, c->rect[2],
		c->rect[3] < s->clip_bottom ? c->rect[3] : s->clip_bottom);
	GLbitfield bits = 0;
	if (c->clear_color) {
		gl->ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		gl->ClearColor((float)(c->argb >> 16 & 255) / 255, (float)(c->argb >> 8 & 255) / 255,
			       (float)(c->argb & 255) / 255, (float)(c->argb >> 24) / 255);
		bits |= GL_COLOR_BUFFER_BIT;
	}
	if (c->clear_depth) {
		gl->DepthMask(GL_TRUE);
		gl->ClearDepth(gl_depth(s, c->z));
		bits |= GL_DEPTH_BUFFER_BIT;
	}
	if (bits)
		gl->Clear(bits);
}

/* Sets up drawing a range with its state */
static void set_state(struct gpu *g, const struct gpu_target *t, const struct raster_state *s, int mode,
		      const struct raster_texture *texture)
{
	struct gl *gl = &g->gl;
	const struct program *p = &g->program[s->perspective_z ? PROGRAM_PERSPECTIVE_Z : PROGRAM_Z];
	gl->UseProgram(p->id);
	gl->Uniform2f(p->size, (float)t->width, (float)t->height);
	gl->Uniform1i(p->mode, mode);
	gl->Uniform1i(p->texture_op, mode == MODE_TEXTURED ? (GLint)s->texture_op : 0);
	gl->Uniform1i(p->fog_mode, mode == MODE_BITMAP ? 0 : (GLint)s->fog_mode);
	gl->Uniform3f(p->fog_color, s->fog_color[1], s->fog_color[2], s->fog_color[3]);
	gl->Uniform1f(p->fog_start, s->fog_start);
	gl->Uniform1f(p->fog_end, s->fog_end);
	gl->Uniform1f(p->fog_density, s->fog_density);
	gl->Uniform1i(p->alpha_test, (GLint)s->alpha_test);
	gl->Uniform1f(p->alpha_ref, s->alpha_ref);
	gl->Uniform1i(p->interpolate, s->blend == RASTER_BLEND_INTERPOLATE);
	bool second = mode == MODE_TEXTURED && s->texture2 && s->texture2->levels;
	gl->Uniform1i(p->second, second);
	gl->ActiveTexture(GL_TEXTURE0 + 1);
	gl->BindTexture(GL_TEXTURE_2D, second ? texture_of(g, s->texture2) : g->blank);
	gl->BindSampler(1, g->sampler[1 * 4 + 3]);     /* (bilinear, clamped) */
	gl->ActiveTexture(GL_TEXTURE0);
	if (texture) {
		gl->BindTexture(GL_TEXTURE_2D, texture_of(g, texture));
		unsigned filter = !s->bilinear ? 0 : s->trilinear ? 2 : 1;
		gl->BindSampler(0, mode == MODE_BITMAP ? g->bitmap_sampler :
				   g->sampler[filter * 4 + (s->clamp_u ? 2 : 0) + (s->clamp_v ? 1 : 0)]);
	} else {
		gl->BindTexture(GL_TEXTURE_2D, g->blank);
		gl->BindSampler(0, g->bitmap_sampler);
	}
	if (s->z_func == RASTER_Z_NONE) {
		gl->Disable(GL_DEPTH_TEST);
	} else {
		gl->Enable(GL_DEPTH_TEST);
		gl->DepthFunc(depth_func(s));
		gl->DepthMask(s->z_write ? GL_TRUE : GL_FALSE);
	}
	switch (s->blend) {
	case RASTER_BLEND_INTERPOLATE:
		gl->BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case RASTER_BLEND_FACTORS:
		gl->BlendFunc(blend_factor(s->blend_src), blend_factor(s->blend_dst));
		break;
	default:
		gl->BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		break;
	}
	scissor(g, t, s->clip_left, s->clip_top, s->clip_right, s->clip_bottom);
}

static bool draw(struct gpu *g, const struct gpu_target *t, const struct raster_state *states,
		 const struct raster_cmd *cmds, unsigned n)
{
	struct gl *gl = &g->gl;
	if (!build(g, states, cmds, n))
		return false;
	gl->BindFramebuffer(GL_FRAMEBUFFER, t->fbo);
	gl->Viewport(0, 0, t->width, t->height);
	gl->BindVertexArray(g->vao);
	gl->BindBuffer(GL_ARRAY_BUFFER, g->vbo);
	gl->BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(g->nverts * FLOATS * sizeof(float)), g->verts, GL_STREAM_DRAW);
	for (unsigned k = 0; k < g->nranges; k++) {
		const struct range *r = &g->ranges[k];
		const struct raster_state *s = &states[r->state];
		if (r->clear) {
			clear(g, t, s, r->clear);
			continue;
		}
		set_state(g, t, s, r->mode, r->texture);
		gl->ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		gl->DrawArrays(GL_TRIANGLES, (GLint)r->first, (GLsizei)r->count);
	}
	return true;
}

static void copy(struct gpu *g, const struct gpu_target *to, const struct gpu_target *from)
{
	struct gl *gl = &g->gl;
	gl->BindFramebuffer(GL_READ_FRAMEBUFFER, from->fbo);
	gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, to->fbo);
	gl->Disable(GL_SCISSOR_TEST);
	gl->BlitFramebuffer(0, 0, from->width, from->height, 0, 0, to->width, to->height,
			    GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	gl->Enable(GL_SCISSOR_TEST);
	gl->BindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void read_back(struct gpu *g, const struct gpu_target *t, uint32_t *color)
{
	struct gl *gl = &g->gl;
	gl->BindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
	gl->ReadBuffer(GL_COLOR_ATTACHMENT0);
	gl->ReadPixels(0, 0, t->width, t->height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, color);
}

/* ---- the thread: getting ready ----------------------------------------------------------- */

/*
 * A small scene drawn by both raster.c and the GPU, which must come out
 * alike, but for edges and filtering: so a driver that gets something
 * wrong isn't used. (raster.c's primitives, not raster_run(): its threads
 * are the emulator's.)
 */
static bool self_test(struct gpu *g, char *why, size_t why_size)
{
	enum { W = 64, H = 48, STATES = 5 };
	static uint32_t texels[8 * 8], cpu_color[W * H], gpu_color[W * H];
	static float cpu_depth[W * H];
	for (unsigned k = 0; k < 8 * 8; k++)
		texels[k] = (k * 37u % 5 ? 0xFF000000u : 0x80000000u) | (k * 2654435761u >> 8 & 0xFFFFFF);
	struct raster_texture tx = { .levels = 1, .width = { 8 }, .height = { 8 }, .texels = { texels } };

	struct raster_state states[STATES];
	memset(states, 0, sizeof states);
	for (int k = 0; k < STATES; k++) {
		states[k].z_func = RASTER_Z_LT;
		states[k].z_write = true;
		states[k].width = 3;
		states[k].clip_right = W;
		states[k].clip_bottom = H;
		states[k].texture = &tx;
	}
	states[1].bilinear = true;
	states[1].texture_op = RASTER_TEX_MODULATE | RASTER_TEX_HIGHLIGHT;
	states[2].texture_op = RASTER_TEX_DECAL;
	states[2].fog_mode = RASTER_FOG_LINEAR;
	states[2].fog_start = 0.5f;
	states[2].fog_end = 2;
	states[2].fog_color[1] = 0.25f;
	states[3].blend = RASTER_BLEND_INTERPOLATE;
	states[3].alpha_test = RASTER_ALPHA_GT;
	states[3].alpha_ref = 0.3f;
	states[4].blend = RASTER_BLEND_FACTORS;
	states[4].blend_src = RASTER_F_SRC_ALPHA;
	states[4].blend_dst = RASTER_F_ONE;
	states[4].perspective_z = true;
	states[4].z_func = RASTER_Z_NONE;

	struct raster_cmd cmds[8];
	memset(cmds, 0, sizeof cmds);
	cmds[0].kind = RASTER_CMD_CLEAR;
	cmds[0].rect[2] = W;
	cmds[0].rect[3] = H;
	cmds[0].clear_color = cmds[0].clear_depth = true;
	cmds[0].argb = 0xFF203040u;
	cmds[0].z = 1;
	static const float tri[5][3][2] = {
		{ { 2.3f, 3.1f }, { 40.7f, 6.2f }, { 9.4f, 41.8f } },
		{ { 20.2f, 2.6f }, { 61.5f, 10.3f }, { 33.1f, 45.2f } },
		{ { 5.5f, 20.5f }, { 50.25f, 30.75f }, { 15.1f, 46.9f } },
		{ { 30.6f, 1.2f }, { 62.4f, 40.6f }, { 28.3f, 30.1f } },
		{ { 1.0f, 44.0f }, { 63.0f, 25.0f }, { 50.0f, 47.0f } },
	};
	for (unsigned k = 0; k < 5; k++) {
		struct raster_cmd *c = &cmds[1 + k];
		c->kind = RASTER_CMD_TRIANGLE;
		c->state = k;
		c->textured = k != 0;
		for (unsigned j = 0; j < 3; j++) {
			struct raster_vertex *v = &c->v[j];
			v->x = tri[k][j][0];
			v->y = tri[k][j][1];
			v->z = 0.2f + 0.15f * (float)k + 0.05f * (float)j;
			v->invw = 0.6f + 0.3f * (float)j;
			v->r = (float)((j + k) % 3) * 0.5f;
			v->g = (float)(j % 2);
			v->b = 0.25f * (float)k;
			v->a = 0.4f + 0.3f * (float)j;
			v->uw = (float)j * 0.7f * v->invw;
			v->vw = (float)(2 - j) * 0.6f * v->invw;
			for (int i = 0; i < 3; i++) {
				v->kd[i] = 0.8f;
				v->ks[i] = 0.1f;
			}
		}
	}
	cmds[6].kind = RASTER_CMD_POINT;
	cmds[6].v[0] = cmds[1].v[0];
	cmds[6].v[0].x = 50.2f;
	cmds[6].v[0].y = 8.7f;
	cmds[6].v[0].z = 0;
	cmds[7].kind = RASTER_CMD_BITMAP;
	cmds[7].state = 3;
	cmds[7].image = &tx;
	cmds[7].v[0].x = 44.4f;
	cmds[7].v[0].y = 36.6f;

	struct raster_target cpu = { W, H, cpu_color, cpu_depth };
	for (unsigned k = 0; k < 8; k++) {
		const struct raster_cmd *c = &cmds[k];
		const struct raster_state *s = &states[c->state];
		switch (c->kind) {
		case RASTER_CMD_CLEAR:
			raster_clear(&cpu, 0, 0, W, H, true, c->argb, true, c->z);
			break;
		case RASTER_CMD_TRIANGLE:
			raster_triangle(&cpu, s, &c->v[0], &c->v[1], &c->v[2], c->textured);
			break;
		case RASTER_CMD_POINT:
			raster_point(&cpu, s, &c->v[0]);
			break;
		case RASTER_CMD_BITMAP:
			raster_bitmap(&cpu, s, c->image, c->v[0].x, c->v[0].y, c->v[0].z);
			break;
		}
	}

	struct gpu_target *t = calloc(1, sizeof *t);
	if (!t)
		return false;
	t->width = W;
	t->height = H;
	bool ok = new_target(g, t) && draw(g, t, states, cmds, 8);
	if (ok) {
		read_back(g, t, gpu_color);
		ok = g->gl.GetError() == GL_NO_ERROR;
	}
	g->gl.DeleteTextures(1, &tx.gpu);
	free_target(g, t);
	if (!ok) {
		snprintf(why, why_size, "OpenGL failed its self-test (drawing)");
		return false;
	}
	unsigned off = 0, worst = 0;
	for (unsigned k = 0; k < W * H; k++) {
		unsigned diff = 0;
		for (int sh = 0; sh < 32; sh += 8) {
			int d = (int)(cpu_color[k] >> sh & 255) - (int)(gpu_color[k] >> sh & 255);
			diff = (unsigned)abs(d) > diff ? (unsigned)abs(d) : diff;
		}
		off += diff > 12;
		worst = diff > worst ? diff : worst;
	}
	debug(g, "self-test: %u of %u pixels off by more than 12 (at most %u)", off, W * H, worst);
	if (off > W * H / 30) {
		snprintf(why, why_size, "OpenGL failed its self-test (%u of %u pixels wrong)", off, W * H);
		return false;
	}
	return true;
}

static bool init(struct gpu *g)
{
	struct gl *gl = &g->gl;
	if (!gl_open(gl, g->allow_software, g->why, sizeof g->why))
		return false;
	GLint tex = 0, rb = 0;
	gl->GetIntegerv(GL_MAX_TEXTURE_SIZE, &tex);
	gl->GetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &rb);
	if (tex < 4096 || rb < 4096) {
		snprintf(g->why, sizeof g->why, "OpenGL on %s draws at most %d pixels across", gl->renderer,
			 tex < rb ? tex : rb);
		return false;
	}
	if (!make_program(g, &g->program[PROGRAM_Z], "") ||
	    !make_program(g, &g->program[PROGRAM_PERSPECTIVE_Z], "#define PERSPECTIVE_Z\n")) {
		snprintf(g->why, sizeof g->why, "OpenGL on %s can't build the shaders", gl->renderer);
		return false;
	}
	if (has_extension(g, "GL_EXT_texture_filter_anisotropic") ||
	    has_extension(g, "GL_ARB_texture_filter_anisotropic")) {
		gl->GetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &g->anisotropy);
		g->anisotropy = g->anisotropy > 8 ? 8 : g->anisotropy;
	}
	make_samplers(g);
	static const uint32_t white = 0xFFFFFFFFu;
	gl->GenTextures(1, &g->blank);
	gl->BindTexture(GL_TEXTURE_2D, g->blank);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	gl->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, &white);
	gl->GenVertexArrays(1, &g->vao);
	gl->BindVertexArray(g->vao);
	gl->GenBuffers(1, &g->vbo);
	gl->BindBuffer(GL_ARRAY_BUFFER, g->vbo);
	static const struct { GLint size, offset; } attrib[5] = { { 4, 0 }, { 4, 4 }, { 2, 8 }, { 3, 10 }, { 3, 13 } };
	for (GLuint k = 0; k < 5; k++) {
		gl->EnableVertexAttribArray(k);
		gl->VertexAttribPointer(k, attrib[k].size, GL_FLOAT, GL_FALSE, FLOATS * sizeof(float),
					(const void *)(uintptr_t)(attrib[k].offset * sizeof(float)));
	}
	gl->Disable(GL_DITHER);
	gl->Disable(GL_CULL_FACE);
	gl->Disable(GL_MULTISAMPLE);
	gl->Disable(GL_FRAMEBUFFER_SRGB);
	gl->Enable(GL_DEPTH_CLAMP);
	gl->Enable(GL_BLEND);
	gl->BlendEquation(GL_FUNC_ADD);
	gl->Enable(GL_SCISSOR_TEST);
	gl->PixelStorei(GL_UNPACK_ALIGNMENT, 4);
	gl->PixelStorei(GL_PACK_ALIGNMENT, 4);
	gl->ActiveTexture(GL_TEXTURE0);
	if (gl->GetError() != GL_NO_ERROR) {
		snprintf(g->why, sizeof g->why, "OpenGL on %s failed setting up", gl->renderer);
		return false;
	}
	if (!self_test(g, g->why, sizeof g->why))
		return false;
	snprintf(g->renderer, sizeof g->renderer, "OpenGL %s (%s)", gl->version, gl->renderer);
	return true;
}

static void cleanup(struct gpu *g)
{
	struct gl *gl = &g->gl;
	if (gl->priv) {
		delete_dead(g);
		for (int k = 0; k < PROGRAMS; k++)
			if (g->program[k].id)
				gl->DeleteProgram(g->program[k].id);
		if (g->sampler[0])
			gl->DeleteSamplers(12, g->sampler);
		if (g->bitmap_sampler)
			gl->DeleteSamplers(1, &g->bitmap_sampler);
		if (g->blank)
			gl->DeleteTextures(1, &g->blank);
		if (g->vbo)
			gl->DeleteBuffers(1, &g->vbo);
		if (g->vao)
			gl->DeleteVertexArrays(1, &g->vao);
		gl_close(gl);
	}
	free(g->verts);
	free(g->ranges);
}

/* ---- the thread: jobs -------------------------------------------------------------------- */

static const char *const job_names[] = { "none", "target", "free", "copy", "draw", "read", "quit" };

static void run_job(struct gpu *g)
{
	struct gl *gl = &g->gl;
	delete_dead(g);
	g->jobs++;
	debug(g, "job %lu: %s%s", g->jobs, job_names[g->job], g->failed ? " (failed: skipped)" : "");
	if (g->job == JOB_TARGET_FREE) {
		free_target(g, g->target);
		return;
	}
	if (g->failed)
		return;
	if (g->fail_at && g->jobs == g->fail_at) {
		fprintf(stderr, "gpu: failing job %lu (SYSHM_GPU_FAIL)\n", g->jobs);
		g->failed = true;
		return;
	}
	bool ok = true;
	switch (g->job) {
	case JOB_TARGET_NEW:
		ok = new_target(g, g->target);
		break;
	case JOB_DRAW:
		ok = draw(g, g->target, g->states, g->cmds, g->n);
		break;
	case JOB_COPY:
		copy(g, g->target, g->from);
		break;
	case JOB_READ:
		read_back(g, g->target, g->color);
		break;
	default:
		break;
	}
	GLenum err = gl->GetError();
	if (!ok || err != GL_NO_ERROR) {
		fprintf(stderr, "gpu: %s failed (GL error 0x%x)\n", job_names[g->job], err);
		g->failed = true;
	}
	g->job_ok = !g->failed;
}

static void *gpu_thread(void *arg)
{
	struct gpu *g = arg;
	bool ok = init(g);
	pthread_mutex_lock(&g->mu);
	g->ready = ok;
	g->init_done = true;
	pthread_cond_broadcast(&g->done);
	pthread_mutex_unlock(&g->mu);
	while (ok) {
		pthread_mutex_lock(&g->mu);
		while (!g->busy)
			pthread_cond_wait(&g->posted, &g->mu);
		pthread_mutex_unlock(&g->mu);
		if (g->job == JOB_QUIT)
			break;
		run_job(g);
		pthread_mutex_lock(&g->mu);
		g->busy = false;
		pthread_cond_broadcast(&g->done);
		pthread_mutex_unlock(&g->mu);
	}
	cleanup(g);
	return NULL;
}

/* ---- the emulator's side ------------------------------------------------------------------ */

static void wait_idle(struct gpu *g)
{
	pthread_mutex_lock(&g->mu);
	while (g->busy)
		pthread_cond_wait(&g->done, &g->mu);
	pthread_mutex_unlock(&g->mu);
}

/* (after wait_idle(): fills in the job, then this hands it over) */
static void post(struct gpu *g, enum job job)
{
	pthread_mutex_lock(&g->mu);
	g->job = job;
	g->busy = true;
	pthread_cond_signal(&g->posted);
	pthread_mutex_unlock(&g->mu);
}

/* Ready, not failed, and nothing in hand */
static bool usable(struct gpu *g)
{
	if (!g)
		return false;
	wait_idle(g);
	return g->init_done && g->ready && !g->failed;
}

struct gpu *gpu_open(bool allow_software)
{
	struct gpu *g = calloc(1, sizeof *g);
	if (!g)
		return NULL;
	g->allow_software = allow_software;
	g->debug = getenv("SYSHM_GPU_DEBUG") != NULL;
	const char *fail = getenv("SYSHM_GPU_FAIL");
	g->fail_at = fail ? strtoul(fail, NULL, 10) : 0;
	pthread_mutex_init(&g->mu, NULL);
	pthread_cond_init(&g->posted, NULL);
	pthread_cond_init(&g->done, NULL);
	if (pthread_create(&g->thread, NULL, gpu_thread, g)) {
		free(g);
		return NULL;
	}
	return g;
}

bool gpu_ready(struct gpu *g, const char **why)
{
	if (!g) {
		if (why)
			*why = "no GPU";
		return false;
	}
	pthread_mutex_lock(&g->mu);
	while (!g->init_done)
		pthread_cond_wait(&g->done, &g->mu);
	pthread_mutex_unlock(&g->mu);
	if (!g->ready && why)
		*why = g->why;
	return g->ready;
}

const char *gpu_renderer(const struct gpu *g)
{
	return g && g->ready ? g->renderer : "no GPU";
}

bool gpu_failed(const struct gpu *g)
{
	if (!g)
		return true;
	wait_idle((struct gpu *)g);
	return g->failed;
}

void gpu_close(struct gpu *g)
{
	if (!g)
		return;
	if (gpu_ready(g, NULL)) {
		wait_idle(g);
		post(g, JOB_QUIT);
	}
	pthread_join(g->thread, NULL);
	pthread_mutex_destroy(&g->mu);
	pthread_cond_destroy(&g->posted);
	pthread_cond_destroy(&g->done);
	free(g->dead);
	free(g);
}

struct gpu_target *gpu_target_new(struct gpu *g, int width, int height)
{
	if (!usable(g) || width <= 0 || height <= 0)
		return NULL;
	struct gpu_target *t = calloc(1, sizeof *t);
	if (!t)
		return NULL;
	t->width = width;
	t->height = height;
	g->target = t;
	post(g, JOB_TARGET_NEW);
	wait_idle(g);
	if (!g->job_ok) {
		gpu_target_free(g, t);
		return NULL;
	}
	return t;
}

void gpu_target_free(struct gpu *g, struct gpu_target *t)
{
	if (!g || !t)
		return;
	wait_idle(g);
	if (!g->init_done || !g->ready) {
		free(t);
		return;
	}
	g->target = t;
	post(g, JOB_TARGET_FREE);
}

void gpu_copy(struct gpu *g, struct gpu_target *to, struct gpu_target *from)
{
	if (!usable(g) || !to || !from || to->width != from->width || to->height != from->height)
		return;
	g->target = to;
	g->from = from;
	post(g, JOB_COPY);
}

void gpu_start(struct gpu *g, struct gpu_target *t, const struct raster_state *states,
	       const struct raster_cmd *cmds, unsigned n)
{
	if (!usable(g) || !t || !n)
		return;
	g->target = t;
	g->states = states;
	g->cmds = cmds;
	g->n = n;
	post(g, JOB_DRAW);
}

void gpu_wait(struct gpu *g)
{
	if (g)
		wait_idle(g);
}

bool gpu_read(struct gpu *g, struct gpu_target *t, uint32_t *color)
{
	if (!usable(g) || !t)
		return false;
	g->target = t;
	g->color = color;
	post(g, JOB_READ);
	wait_idle(g);
	return g->job_ok;
}

void gpu_texture_free(struct gpu *g, struct raster_texture *tex)
{
	if (!g || !tex->gpu)
		return;
	wait_idle(g);
	pthread_mutex_lock(&g->mu);
	if (g->ndead == g->dead_cap) {
		unsigned cap = g->dead_cap ? 2 * g->dead_cap : 64;
		GLuint *d = realloc(g->dead, cap * sizeof *d);
		if (d) {
			g->dead = d;
			g->dead_cap = cap;
		}
	}
	if (g->ndead < g->dead_cap)
		g->dead[g->ndead++] = tex->gpu;
	pthread_mutex_unlock(&g->mu);
	tex->gpu = 0;
}
