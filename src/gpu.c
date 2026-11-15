#include "gpu.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gl.h"

/* ---- shaders ------------------------------------------------------------------------- */

/*
 * Everything is drawn with w = 1, so the GPU interpolates linearly in screen
 * space, as raster.c does; texture coordinates and fog divide by 1/w (or a
 * unit's q/w) per pixel. (The varyings are smooth, which with w = 1 is the
 * same as noperspective: Mesa's llvmpipe interpolates noperspective ones
 * wrongly where it clips a triangle at the target's edges, which OpenGL's
 * do reach.) Rows go upward in GL, so image row y is GL row y: drawn upside
 * down, read back the right way up.
 */
static const char vertex_source[] =
	"layout(location = 0) in vec4 a_pos;\n"          /* x, y (pixels), z, 1/w */
	"layout(location = 1) in vec4 a_color0;\n"
	"layout(location = 2) in vec4 a_color1;\n"
	"layout(location = 3) in vec4 a_color2;\n"
	"layout(location = 4) in float a_fog;\n"
	"layout(location = 5) in vec3 a_tex0;\n"         /* s/w, t/w, q/w (bitmaps: texel coordinates) */
	"layout(location = 6) in vec3 a_tex1;\n"
	"layout(location = 7) in vec2 a_texr;\n"         /* each unit's r/w (3D textures) */
	"uniform vec2 u_size;\n"
	"smooth out float v_invw;\n"
	"smooth out vec4 v_color0;\n"
	"smooth out vec4 v_color1;\n"
	"smooth out vec4 v_color2;\n"
	"smooth out float v_fog;\n"
	"smooth out vec3 v_tex0;\n"
	"smooth out vec3 v_tex1;\n"
	"smooth out vec2 v_texr;\n"
	"void main()\n"
	"{\n"
	"	gl_Position = vec4(a_pos.x * 2.0 / u_size.x - 1.0, a_pos.y * 2.0 / u_size.y - 1.0,\n"
	"			   a_pos.z * 2.0 - 1.0, 1.0);\n"
	"	v_invw = a_pos.w;\n"
	"	v_color0 = a_color0;\n"
	"	v_color1 = a_color1;\n"
	"	v_color2 = a_color2;\n"
	"	v_fog = a_fog;\n"
	"	v_tex0 = a_tex0;\n"
	"	v_tex1 = a_tex1;\n"
	"	v_texr = a_texr;\n"
	"}\n";

/*
 * raster.c's fragment(), step for step, made for each way of drawing (a
 * program): MODE 0 color 0, 1 textured, 2 bitmaps, 3 pixels; PERSPECTIVE_Z;
 * textured, UNIT0 and UNIT1 (sampled), PROJECTIVE0 and PROJECTIVE1, and
 * VOLUME0 and VOLUME1 (3D textures), and the stages' code between the head
 * and the tail (see stage_code()).
 */
static const char fragment_head[] =
	"smooth in float v_invw;\n"
	"smooth in vec4 v_color0;\n"
	"smooth in vec4 v_color1;\n"
	"smooth in vec4 v_color2;\n"
	"smooth in float v_fog;\n"
	"smooth in vec3 v_tex0;\n"
	"smooth in vec3 v_tex1;\n"
	"smooth in vec2 v_texr;\n"
	"#ifdef VOLUME0\n"
	"uniform sampler3D u_texture0;\n"
	"#else\n"
	"uniform sampler2D u_texture0;\n"
	"#endif\n"
	"#ifdef VOLUME1\n"
	"uniform sampler3D u_texture1;\n"
	"#else\n"
	"uniform sampler2D u_texture1;\n"
	"#endif\n"
	"uniform vec4 u_constant[STAGES];\n"
	"uniform int u_fog_mode;\n"
	"uniform bool u_fog_coord;\n"
	"uniform vec3 u_fog_color;\n"
	"uniform float u_fog_start, u_fog_end, u_fog_density;\n"
	"uniform int u_alpha_test;\n"
	"uniform float u_alpha_ref;\n"
	"uniform int u_transparent;\n"                   /* alpha 0: drawn (0), skipped adding nothing (1), skipped (2) */
	"uniform vec2 u_pix_origin, u_pix_inv, u_pix_size;\n"   /* pixels: as raster.c's pixels() */
	"uniform int u_pix_kind, u_pix_bit;\n"
	"uniform vec4 u_pix_color;\n"
	"uniform float u_pix_z;\n"
	"layout(location = 0) out vec4 o_color;\n"
	"void main()\n"
	"{\n"
	"#ifdef PERSPECTIVE_Z\n"
	"	gl_FragDepth = v_invw > 0.0 ? v_invw / (1.0 + v_invw) : 0.0;\n"
	"#endif\n"
	"	float w = v_invw != 0.0 ? 1.0 / v_invw : 1e30;\n"
	"	vec4 c = v_color0;\n"
	"#if MODE == 1\n"
	"#ifdef UNIT0\n"
	"#ifdef PROJECTIVE0\n"
	"	float d0 = v_tex0.z != 0.0 ? 1.0 / v_tex0.z : 1e30;\n"
	"#else\n"
	"	float d0 = w;\n"
	"#endif\n"
	"#ifdef VOLUME0\n"
	"	vec4 t0 = texture(u_texture0, vec3(v_tex0.xy, v_texr.x) * d0);\n"
	"#else\n"
	"	vec4 t0 = texture(u_texture0, v_tex0.xy * d0);\n"
	"#endif\n"
	"#endif\n"
	"#ifdef UNIT1\n"
	"#ifdef PROJECTIVE1\n"
	"	float d1 = v_tex1.z != 0.0 ? 1.0 / v_tex1.z : 1e30;\n"
	"#else\n"
	"	float d1 = w;\n"
	"#endif\n"
	"#ifdef VOLUME1\n"
	"	vec4 t1 = texture(u_texture1, vec3(v_tex1.xy, v_texr.y) * d1);\n"
	"#else\n"
	"	vec4 t1 = texture(u_texture1, v_tex1.xy * d1);\n"
	"#endif\n"
	"#endif\n";

static const char fragment_tail[] =
	"#elif MODE == 2\n"
	"	c = texelFetch(u_texture0, ivec2(floor(v_tex0.xy)), 0);\n"
	"	if (c.a <= 0.0)\n"
	"		discard;\n"
	"#elif MODE == 3\n"
	"	vec2 f = floor((gl_FragCoord.xy - u_pix_origin) * u_pix_inv);\n"
	"	if (f.x < 0.0 || f.y < 0.0 || f.x >= u_pix_size.x || f.y >= u_pix_size.y)\n"
	"		discard;\n"
	"	vec4 p = texelFetch(u_texture0, ivec2(f), 0);\n"
	"	uvec4 b = uvec4(p * 255.0 + 0.5);\n"
	"	gl_FragDepth = u_pix_z;\n"
	"	c = u_pix_color;\n"
	"	if (u_pix_kind == 0) {\n"
	"		c = p;\n"
	"	} else if (u_pix_kind == 1) {\n"
	"		if (all(equal(b, uvec4(0u))))\n"
	"			discard;\n"
	"	} else if (u_pix_kind == 2) {\n"
	"		gl_FragDepth = clamp(uintBitsToFloat(b.a << 24u | b.r << 16u | b.g << 8u | b.b), 0.0, 1.0);\n"
	"	} else if (u_pix_bit >= 0 && (b.b >> uint(u_pix_bit) & 1u) == 0u) {\n"
	"		discard;\n"
	"	}\n"
	"#endif\n"
	"	if (u_fog_mode != 0) {\n"
	"		float f, z = u_fog_coord ? v_fog * w : w, d = u_fog_density;\n"
	"		if (u_fog_mode == 1) {\n"
	"			f = c.a;\n"
	"			c.a = 1.0;\n"
	"		} else if (u_fog_mode == 2) {\n"
	"			f = z < u_fog_start ? 1.0 : z > u_fog_end ? 0.0 : (u_fog_end - z) / (u_fog_end - u_fog_start);\n"
	"		} else if (u_fog_mode == 3) {\n"
	"			f = exp(-d * z);\n"
	"		} else {\n"
	"			f = exp(-(d * z) * (d * z));\n"
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
	"	if (u_transparent != 0 && c.a <= 0.0 && (u_transparent == 2 || all(lessThanEqual(c.rgb, vec3(0.0)))))\n"
	"		discard;\n"
	"	o_color = c;\n"
	"}\n";

#define STR_(x) #x
#define STR(x) STR_(x)

enum { MODE_COLOR, MODE_TEXTURED, MODE_BITMAP, MODE_PIXELS };

/* What a program draws: what it's made for (all but the stages of textured drawing zero) */
struct program_key {
	uint8_t mode, perspective_z, units, projective, volume, stages;
	struct raster_stage stage[RASTER_STAGES];       /* (but for their constants, which are uniforms) */
};

struct program {
	struct program_key key;
	GLuint id;
	GLint size, texture0, texture1, constant, fog_mode, fog_coord, fog_color, fog_start, fog_end, fog_density,
	      alpha_test, alpha_ref, transparent, pix_origin, pix_inv, pix_size, pix_kind, pix_bit, pix_color, pix_z;
};

/* per vertex: x, y, z, 1/w, the colors (r, g, b, a), fog, each unit's s/w, t/w, q/w, then each's r/w */
#define FLOATS (4 + 4 * RASTER_COLORS + 1 + 4 * RASTER_UNITS)
_Static_assert(RASTER_UNITS == 2 && RASTER_COLORS == 3 && RASTER_CONSTANT == 6, "the shaders' units and sources");

/* ---- the backend ----------------------------------------------------------------- */

struct gpu_target {
	int width, height;
	GLuint fbo, color, depth;
};

enum job { JOB_NONE, JOB_TARGET_NEW, JOB_TARGET_FREE, JOB_COPY, JOB_DRAW, JOB_READ, JOB_READ_DEPTH, JOB_QUIT };

/* Commands cut into draw calls: consecutive ones that draw the same way */
struct range {
	unsigned first, count;          /* vertices (a clear: none) */
	const struct raster_cmd *clear;
	unsigned state;
	int mode;
	const struct raster_texture *image;     /* (bitmaps, pixels) */
	const struct raster_cmd *pixels;        /* (pixels: the command) */
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
	/* host time in drawing jobs and reading back, textures made (their texels' bytes): for profiling */
	_Atomic uint64_t draw_ns, read_ns, uploads, upload_bytes;
	/* the job in hand (one at a time) */
	bool busy;
	enum job job;
	struct gpu_target *target, *from;
	const struct raster_state *states;
	const struct raster_cmd *cmds;
	unsigned n;
	uint32_t *color;
	float *depth_out;
	uint8_t *stencil_out;
	bool job_ok;
	/* GL textures to delete (from gpu_texture_free()) */
	GLuint *dead;
	unsigned ndead, dead_cap;

	/* the thread's */
	struct gl gl;
	struct program *programs;       /* made as they're needed */
	unsigned nprograms, last_program;
	GLuint vao, vbo;
	GLuint sampler[6][2][8][5];     /* by min and mag filter, clamping (s * 4 + t * 2 + r), anisotropy (1 << k) */
	GLuint bitmap_sampler;
	GLuint blank;                   /* (bound when drawing without a texture: drivers want one) */
	GLenum blend_eq;                /* as last set (changing them costs, on some drivers) */
	float blend_color[4];
	bool stencil;                   /* (the stencil test on) */
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

static GLuint compile(struct gpu *g, GLenum type, const char *defines, const char *head, const char *code,
		      const char *tail)
{
	struct gl *gl = &g->gl;
	GLuint s = gl->CreateShader(type);
	const GLchar *src[6] = { "#version 330 core\n", "#define STAGES " STR(RASTER_STAGES) "\n", defines, head, code,
				 tail };
	gl->ShaderSource(s, 6, src, NULL);
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

/* Text added to a buffer (as much as fits) */
static void append(char *out, size_t size, size_t *n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (*n < size) {
		int k = vsnprintf(out + *n, size - *n, fmt, ap);
		*n += k > 0 ? (size_t)k : 0;
	}
	va_end(ap);
}

/* An argument of stage j as GLSL (a vec4) */
static void arg_code(char *out, size_t size, size_t *n, unsigned arg, unsigned j)
{
	static const char *const source[RASTER_CONSTANT] = { "c", "v_color0", "v_color1", "v_color2", "t0", "t1" };
	unsigned src = arg & 15u;
	append(out, size, n, arg & RASTER_INVERT ? "(1.0 - " : "");
	append(out, size, n, arg & RASTER_ALPHA ? "vec4(" : "");
	if (src == RASTER_CONSTANT)
		append(out, size, n, "u_constant[%u]", j);
	else
		append(out, size, n, "%s", src < RASTER_CONSTANT ? source[src] : "c");
	append(out, size, n, arg & RASTER_ALPHA ? ".a)" : "");
	append(out, size, n, arg & RASTER_INVERT ? ")" : "");
}

/* The stages as GLSL statements, each making c from c: its .rgb from group 0's operation, its .a from 1's */
static void stage_code(const struct program_key *k, char *out, size_t size)
{
	size_t n = 0;
	out[0] = 0;
	for (unsigned j = 0; j < k->stages; j++) {
		const struct raster_stage *st = &k->stage[j];
		append(out, size, &n, "\tc = vec4(");
		for (int g = 0; g < 2; g++) {
			static const char *const op[] = { "#0", "#0 * #1", "#0 + #1", "#0 + #1 - 0.5",
							  "#0 * #2 + #1 * (1.0 - #2)", "#0 - #1" };
			append(out, size, &n, "%s%s((", g ? ", " : "", st->clamp[g] ? "clamp" : "");
			for (const char *p = op[st->op[g] < 6 ? st->op[g] : 0]; *p; p++) {
				if (*p == '#')
					arg_code(out, size, &n, st->arg[g][*++p - '0'], j);
				else
					append(out, size, &n, "%c", *p);
			}
			append(out, size, &n, ")%s", g ? ".a" : ".rgb");
			if (st->shift[g])
				append(out, size, &n, " * %u.0", 1u << st->shift[g]);
			append(out, size, &n, st->clamp[g] ? ", 0.0, 1.0)" : ")");
		}
		append(out, size, &n, ");\n");
	}
}

/* The program that draws as the key says, made the first time it's asked for; NULL if it can't be */
static struct program *program_of(struct gpu *g, const struct program_key *key)
{
	if (g->last_program < g->nprograms && !memcmp(&g->programs[g->last_program].key, key, sizeof *key))
		return &g->programs[g->last_program];
	for (unsigned k = 0; k < g->nprograms; k++)
		if (!memcmp(&g->programs[k].key, key, sizeof *key)) {
			g->last_program = k;
			return &g->programs[k];
		}
	struct program *ps = realloc(g->programs, (g->nprograms + 1) * sizeof *ps);
	if (!ps)
		return NULL;
	g->programs = ps;
	struct program *p = &ps[g->nprograms];
	memset(p, 0, sizeof *p);
	p->key = *key;

	struct gl *gl = &g->gl;
	char defines[256], code[4096];
	snprintf(defines, sizeof defines, "#define MODE %u\n%s%s%s%s%s%s%s", key->mode,
		 key->perspective_z ? "#define PERSPECTIVE_Z\n" : "", key->units & 1 ? "#define UNIT0\n" : "",
		 key->units & 2 ? "#define UNIT1\n" : "", key->projective & 1 ? "#define PROJECTIVE0\n" : "",
		 key->projective & 2 ? "#define PROJECTIVE1\n" : "", key->volume & 1 ? "#define VOLUME0\n" : "",
		 key->volume & 2 ? "#define VOLUME1\n" : "");
	stage_code(key, code, sizeof code);
	debug(g, "program %u: %s%s", g->nprograms, defines, code);
	GLuint vs = compile(g, GL_VERTEX_SHADER, "", vertex_source, "", "");
	GLuint fs = compile(g, GL_FRAGMENT_SHADER, defines, fragment_head, code, fragment_tail);
	if (!vs || !fs) {
		if (vs)
			gl->DeleteShader(vs);
		if (fs)
			gl->DeleteShader(fs);
		return NULL;
	}
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
		gl->DeleteProgram(p->id);
		return NULL;
	}
#define U(field, name) p->field = gl->GetUniformLocation(p->id, name)
	U(size, "u_size");
	U(texture0, "u_texture0");
	U(texture1, "u_texture1");
	U(constant, "u_constant");
	U(fog_mode, "u_fog_mode");
	U(fog_coord, "u_fog_coord");
	U(fog_color, "u_fog_color");
	U(fog_start, "u_fog_start");
	U(fog_end, "u_fog_end");
	U(fog_density, "u_fog_density");
	U(alpha_test, "u_alpha_test");
	U(alpha_ref, "u_alpha_ref");
	U(transparent, "u_transparent");
	U(pix_origin, "u_pix_origin");
	U(pix_inv, "u_pix_inv");
	U(pix_size, "u_pix_size");
	U(pix_kind, "u_pix_kind");
	U(pix_bit, "u_pix_bit");
	U(pix_color, "u_pix_color");
	U(pix_z, "u_pix_z");
#undef U
	gl->UseProgram(p->id);
	gl->Uniform1i(p->texture0, 0);
	gl->Uniform1i(p->texture1, 1);
	g->last_program = g->nprograms++;
	return p;
}

/* What draws a range */
static void key_of(const struct raster_state *s, int mode, struct program_key *k)
{
	memset(k, 0, sizeof *k);
	k->mode = (uint8_t)mode;
	k->perspective_z = s->perspective_z;
	if (mode != MODE_TEXTURED)
		return;
	k->units = (uint8_t)raster_units_used(s);
	for (unsigned u = 0; u < RASTER_UNITS; u++) {
		k->projective |= (uint8_t)(k->units >> u & 1 && s->unit[u].projective ? 1u << u : 0);
		k->volume |= (uint8_t)(k->units >> u & 1 && s->unit[u].texture->depth[0] ? 1u << u : 0);
	}
	k->stages = (uint8_t)(s->stages < RASTER_STAGES ? s->stages : RASTER_STAGES);
	for (unsigned j = 0; j < k->stages; j++) {
		const struct raster_stage *st = &s->stage[j];
		struct raster_stage *ks = &k->stage[j];
		for (int g = 0; g < 2; g++) {
			unsigned op = st->op[g] <= RASTER_OP_SUBTRACT ? st->op[g] : RASTER_OP_REPLACE;
			unsigned args = op == RASTER_OP_REPLACE ? 1 : op == RASTER_OP_INTERPOLATE ? 3 : 2;
			ks->op[g] = (uint8_t)op;
			for (unsigned n = 0; n < args; n++)
				ks->arg[g][n] = st->arg[g][n] & (15u | RASTER_ALPHA | RASTER_INVERT);
			ks->shift[g] = st->shift[g] < 3 ? st->shift[g] : 2;
			ks->clamp[g] = st->clamp[g];
		}
	}
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
 * A unit's sampler: its filters and clamping, and as much anisotropic
 * filtering as it allows, where there's some (made the first time it's
 * needed). The GPU picks mipmap levels per pixel.
 */
static GLuint sampler_of(struct gpu *g, const struct raster_unit *u)
{
	static const GLint filter[6] = { GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST,
					 GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR };
	unsigned min = u->min_filter < 6 ? u->min_filter : 0, mag = u->mag_filter == RASTER_LINEAR;
	unsigned clamp = (u->clamp_s ? 4u : 0) | (u->clamp_t ? 2u : 0) | (u->clamp_r ? 1u : 0);
	float most = u->anisotropy < g->anisotropy ? u->anisotropy : g->anisotropy;
	unsigned aniso = 0;
	while (aniso < 4 && most >= (float)(2u << aniso))
		aniso++;
	GLuint *s = &g->sampler[min][mag][clamp][aniso];
	if (*s)
		return *s;
	struct gl *gl = &g->gl;
	gl->GenSamplers(1, s);
	gl->SamplerParameteri(*s, GL_TEXTURE_MIN_FILTER, filter[min]);
	gl->SamplerParameteri(*s, GL_TEXTURE_MAG_FILTER, filter[mag]);
	gl->SamplerParameteri(*s, GL_TEXTURE_WRAP_S, clamp & 4 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
	gl->SamplerParameteri(*s, GL_TEXTURE_WRAP_T, clamp & 2 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
	gl->SamplerParameteri(*s, GL_TEXTURE_WRAP_R, clamp & 1 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
	if (aniso)
		gl->SamplerParameterf(*s, GL_TEXTURE_MAX_ANISOTROPY, (float)(1u << aniso));
	return *s;
}

/* A texture's GL copy, made the first time it's drawn with (its texels never change) */
static GLuint texture_of(struct gpu *g, const struct raster_texture *ct)
{
	struct raster_texture *t = (struct raster_texture *)ct;
	if (t->gpu)
		return t->gpu;
	struct gl *gl = &g->gl;
	GLenum target = t->depth[0] ? GL_TEXTURE_3D : GL_TEXTURE_2D;
	gl->GenTextures(1, &t->gpu);
	gl->BindTexture(target, t->gpu);
	/* (a level not half the one before would leave the texture incomplete: those from it go) */
	unsigned levels = 1;
	while (levels < t->levels && t->width[levels] == (t->width[levels - 1] > 1 ? t->width[levels - 1] / 2 : 1) &&
	       t->height[levels] == (t->height[levels - 1] > 1 ? t->height[levels - 1] / 2 : 1) &&
	       t->depth[levels] == (t->depth[levels - 1] > 1 ? t->depth[levels - 1] / 2 : t->depth[levels - 1]))
		levels++;
	gl->TexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
	gl->TexParameteri(target, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
	atomic_fetch_add(&g->uploads, 1);
	for (unsigned l = 0; l < levels; l++) {
		atomic_fetch_add(&g->upload_bytes, 4ull * t->width[l] * t->height[l] * (t->depth[l] ? t->depth[l] : 1));
		if (t->depth[0])
			gl->TexImage3D(GL_TEXTURE_3D, (GLint)l, GL_RGBA8, (GLsizei)t->width[l], (GLsizei)t->height[l],
				       (GLsizei)t->depth[l], 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, t->texels[l]);
		else
			gl->TexImage2D(GL_TEXTURE_2D, (GLint)l, GL_RGBA8, (GLsizei)t->width[l], (GLsizei)t->height[l], 0,
				       GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, t->texels[l]);
	}
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
	gl->RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, t->width, t->height);
	gl->GenFramebuffers(1, &t->fbo);
	gl->BindFramebuffer(GL_FRAMEBUFFER, t->fbo);
	gl->FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t->color);
	gl->FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->depth);
	if (gl->CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return false;
	gl->Disable(GL_SCISSOR_TEST);
	gl->ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	gl->DepthMask(GL_TRUE);
	gl->StencilMask(0xFF);
	gl->ClearColor(0, 0, 0, 0);
	gl->ClearDepth(0);
	gl->ClearStencil(0);
	gl->Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
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

static GLenum stencil_op(unsigned op)
{
	static const GLenum gl[] = { GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP,
				     GL_DECR_WRAP };
	return op < sizeof gl / sizeof gl[0] ? gl[op] : GL_KEEP;
}

static GLenum blend_factor(unsigned f)
{
	static const GLenum gl[] = { GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA,
				     GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR,
				     GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE, GL_CONSTANT_COLOR,
				     GL_ONE_MINUS_CONSTANT_COLOR, GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA };
	return f < sizeof gl / sizeof gl[0] ? gl[f] : GL_ONE;
}

static GLenum blend_equation(unsigned eq)
{
	static const GLenum gl[] = { GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX };
	return eq < sizeof gl / sizeof gl[0] ? gl[eq] : GL_FUNC_ADD;
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
	p[0] = x;
	p[1] = y;
	p[2] = v->z;
	p[3] = v->invw;
	memcpy(p + 4, v->color, sizeof v->color);
	p[4 + 4 * RASTER_COLORS] = v->fog;
	memcpy(p + 5 + 4 * RASTER_COLORS, v->tex, sizeof v->tex);
	memcpy(p + 5 + 4 * RASTER_COLORS + 3 * RASTER_UNITS, v->texr, sizeof v->texr);
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
		for (int j = 0; j < 4; j++)
			v.color[0][j] = v0->color[0][j] + (v1->color[0][j] - v0->color[0][j]) * f;
		v.fog = v0->fog + (v1->fog - v0->fog) * f;
		if (!put_square(g, s, &v, v0->x + dx * f, v0->y + dy * f))
			return false;
	}
	return true;
}

/* The pixels a pixels command may draw (as raster.c's pixel_span(): the shader finds each one's texel) */
static bool put_pixels(struct gpu *g, const struct raster_cmd *c)
{
	const struct raster_texture *im = c->image;
	float x0 = c->v[0].x, y0 = c->v[0].y, x1 = x0 + c->v[1].x * (float)im->width[0],
	      y1 = y0 + c->v[1].y * (float)im->height[0];
	if (!(fabsf(x0) < 1e6f && fabsf(y0) < 1e6f && fabsf(x1) < 1e7f && fabsf(y1) < 1e7f))
		return true;
	return put_rect(g, &c->v[0], floorf(fminf(x0, x1)) - 1, floorf(fminf(y0, y1)) - 1, ceilf(fmaxf(x0, x1)) + 1,
			ceilf(fmaxf(y0, y1)) + 1);
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
		v.tex[0][0] = corner[k][0] * w;
		v.tex[0][1] = corner[k][1] * h;
		put_vertex(p + k * FLOATS, &v, x0 + v.tex[0][0], y0 + v.tex[0][1]);
	}
	return true;
}

static bool add_range(struct gpu *g, unsigned first, const struct raster_cmd *clear, unsigned state, int mode,
		      const struct raster_texture *image, const struct raster_cmd *pixels)
{
	unsigned count = (unsigned)g->nverts - first;
	if (!clear && !count)
		return true;
	struct range *last = g->nranges ? &g->ranges[g->nranges - 1] : NULL;
	if (!clear && !pixels && last && !last->clear && last->state == state && last->mode == mode &&
	    last->image == image && last->first + last->count == first) {
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
	g->ranges[g->nranges++] = (struct range){ first, count, clear, state, mode, image, pixels };
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
		const struct raster_texture *image = NULL;
		bool ok = true;
		switch (c->kind) {
		case RASTER_CMD_TRIANGLE: {
			if (raster_textured(s, c->textured))
				mode = MODE_TEXTURED;
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
			image = c->image;
			ok = put_bitmap(g, c->image, c);
			break;
		case RASTER_CMD_PIXELS:
			if (!c->image || !c->image->levels || !c->image->width[0] || !c->image->height[0] ||
			    !(c->v[1].x != 0 && c->v[1].y != 0))
				continue;
			mode = MODE_PIXELS;
			image = c->image;
			ok = put_pixels(g, c);
			break;
		case RASTER_CMD_CLEAR:
			ok = add_range(g, first, c, c->state, 0, NULL, NULL);
			continue;
		default:
			continue;
		}
		if (!ok || !add_range(g, first, NULL, c->state, mode, image, mode == MODE_PIXELS ? c : NULL))
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

static void color_mask(struct gpu *g, uint32_t keep)
{
	g->gl.ColorMask(!(keep & 0x00FF0000u), !(keep & 0x0000FF00u), !(keep & 0x000000FFu), !(keep & 0xFF000000u));
}

static void clear(struct gpu *g, const struct gpu_target *t, const struct raster_state *s, const struct raster_cmd *c)
{
	struct gl *gl = &g->gl;
	/* (as raster.c: the rectangle's rows within the clip's) */
	scissor(g, t, c->rect[0], c->rect[1] > s->clip_top ? c->rect[1] : s->clip_top, c->rect[2],
		c->rect[3] < s->clip_bottom ? c->rect[3] : s->clip_bottom);
	GLbitfield bits = 0;
	if (c->clear_color) {
		color_mask(g, s->color_keep);
		gl->ClearColor((float)(c->argb >> 16 & 255) / 255, (float)(c->argb >> 8 & 255) / 255,
			       (float)(c->argb & 255) / 255, (float)(c->argb >> 24) / 255);
		bits |= GL_COLOR_BUFFER_BIT;
	}
	if (c->clear_depth) {
		gl->DepthMask(GL_TRUE);
		gl->ClearDepth(gl_depth(s, c->z));
		bits |= GL_DEPTH_BUFFER_BIT;
	}
	if (c->clear_stencil) {
		gl->StencilMask(s->stencil_write);
		gl->ClearStencil(c->stencil);
		bits |= GL_STENCIL_BUFFER_BIT;
	}
	if (bits)
		gl->Clear(bits);
}

/* Sets up drawing a range with its state; false if there's no program for it */
static bool set_state(struct gpu *g, const struct gpu_target *t, const struct raster_state *s, int mode,
		      const struct raster_texture *image)
{
	struct gl *gl = &g->gl;
	struct program_key key;
	key_of(s, mode, &key);
	const struct program *p = program_of(g, &key);
	if (!p)
		return false;
	gl->UseProgram(p->id);
	gl->Uniform2f(p->size, (float)t->width, (float)t->height);
	unsigned units = key.units;
	if (key.stages && p->constant >= 0) {
		GLfloat constant[RASTER_STAGES * 4];
		for (unsigned k = 0; k < key.stages; k++)
			for (int j = 0; j < 4; j++)
				constant[4 * k + j] = s->stage[k].constant[(j + 1) & 3];      /* (r, g, b, a) */
		gl->Uniform4fv(p->constant, (GLsizei)key.stages, constant);
	}
	gl->Uniform1i(p->fog_mode, mode == MODE_BITMAP || mode == MODE_PIXELS ? 0 : (GLint)s->fog_mode);
	gl->Uniform1i(p->fog_coord, s->fog_coord);
	gl->Uniform3f(p->fog_color, s->fog_color[1], s->fog_color[2], s->fog_color[3]);
	gl->Uniform1f(p->fog_start, s->fog_start);
	gl->Uniform1f(p->fog_end, s->fog_end);
	gl->Uniform1f(p->fog_density, s->fog_density);
	gl->Uniform1i(p->alpha_test, (GLint)s->alpha_test);
	gl->Uniform1f(p->alpha_ref, s->alpha_ref);
	gl->Uniform1i(p->transparent, s->all_fragments ? 0 : s->blend == RASTER_BLEND_INTERPOLATE ? 2 : 1);
	for (unsigned k = 0; k < RASTER_UNITS; k++) {
		gl->ActiveTexture(GL_TEXTURE0 + k);
		if (units >> k & 1) {
			gl->BindTexture(key.volume >> k & 1 ? GL_TEXTURE_3D : GL_TEXTURE_2D, texture_of(g, s->unit[k].texture));
			gl->BindSampler(k, sampler_of(g, &s->unit[k]));
		} else if (k == 0 && image) {
			gl->BindTexture(GL_TEXTURE_2D, texture_of(g, image));
			gl->BindSampler(k, g->bitmap_sampler);
		} else {
			gl->BindTexture(GL_TEXTURE_2D, g->blank);
			gl->BindSampler(k, g->bitmap_sampler);
		}
	}
	gl->ActiveTexture(GL_TEXTURE0);
	if (s->z_func == RASTER_Z_NONE) {
		gl->Disable(GL_DEPTH_TEST);
	} else {
		gl->Enable(GL_DEPTH_TEST);
		gl->DepthFunc(depth_func(s));
		gl->DepthMask(s->z_write ? GL_TRUE : GL_FALSE);
	}
	if (s->stencil_func == RASTER_Z_NONE) {
		if (g->stencil)
			gl->Disable(GL_STENCIL_TEST);
		g->stencil = false;
	} else {
		static const GLenum funcs[] = { GL_ALWAYS, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL,
						GL_GEQUAL, GL_ALWAYS, GL_NEVER };
		gl->Enable(GL_STENCIL_TEST);
		g->stencil = true;
		gl->StencilFunc(funcs[s->stencil_func <= RASTER_Z_FALSE ? s->stencil_func : 0], s->stencil_ref,
				s->stencil_mask);
		gl->StencilOp(stencil_op(s->stencil_op[0]), stencil_op(s->stencil_op[1]), stencil_op(s->stencil_op[2]));
		gl->StencilMask(s->stencil_write);
	}
	GLenum eq = GL_FUNC_ADD;
	switch (s->blend) {
	case RASTER_BLEND_INTERPOLATE:
		gl->BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		break;
	case RASTER_BLEND_FACTORS:
		eq = blend_equation(s->blend_eq);
		gl->BlendFunc(blend_factor(s->blend_src), blend_factor(s->blend_dst));
		if (memcmp(g->blend_color, s->blend_color, sizeof g->blend_color)) {
			memcpy(g->blend_color, s->blend_color, sizeof g->blend_color);
			gl->BlendColor(s->blend_color[1], s->blend_color[2], s->blend_color[3], s->blend_color[0]);
		}
		break;
	case RASTER_BLEND_NONE:
		gl->BlendFunc(GL_ONE, GL_ZERO);
		break;
	default:
		gl->BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		break;
	}
	if (eq != g->blend_eq) {
		g->blend_eq = eq;
		gl->BlendEquation(eq);
	}
	color_mask(g, s->color_keep);
	scissor(g, t, s->clip_left, s->clip_top, s->clip_right, s->clip_bottom);
	return true;
}

/*
 * A pixels range: its uniforms, then drawn; a stencil image is drawn into
 * the stencil, its bits within the write mask cleared, then each set one at a
 * time (without the shader stencil export core OpenGL lacks)
 */
static void draw_pixels(struct gpu *g, const struct raster_state *s, const struct range *r)
{
	struct gl *gl = &g->gl;
	const struct program *p = &g->programs[g->last_program];
	const struct raster_cmd *c = r->pixels;
	gl->Uniform2f(p->pix_origin, c->v[0].x, c->v[0].y);
	gl->Uniform2f(p->pix_inv, 1 / c->v[1].x, 1 / c->v[1].y);
	gl->Uniform2f(p->pix_size, (float)c->image->width[0], (float)c->image->height[0]);
	gl->Uniform1i(p->pix_kind, c->pixels);
	gl->Uniform1i(p->pix_bit, -1);
	const float *rc = c->v[0].color[0];
	gl->Uniform4f(p->pix_color, rc[0], rc[1], rc[2], rc[3]);
	gl->Uniform1f(p->pix_z, (float)gl_depth(s, c->v[0].z));
	if (c->pixels != RASTER_PIXELS_STENCIL) {
		gl->DrawArrays(GL_TRIANGLES, (GLint)r->first, (GLsizei)r->count);
		return;
	}
	gl->Uniform1i(p->alpha_test, 0);
	gl->Uniform1i(p->transparent, 0);
	gl->ColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	gl->Disable(GL_DEPTH_TEST);
	gl->Enable(GL_STENCIL_TEST);
	g->stencil = true;
	gl->StencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	gl->StencilFunc(GL_ALWAYS, 0, 0xFF);
	gl->StencilMask(s->stencil_write);
	gl->DrawArrays(GL_TRIANGLES, (GLint)r->first, (GLsizei)r->count);
	gl->StencilFunc(GL_ALWAYS, 0xFF, 0xFF);
	for (int b = 0; b < 8; b++)
		if (s->stencil_write >> b & 1) {
			gl->StencilMask(1u << b);
			gl->Uniform1i(p->pix_bit, b);
			gl->DrawArrays(GL_TRIANGLES, (GLint)r->first, (GLsizei)r->count);
		}
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
		if (!set_state(g, t, s, r->mode, r->image))
			return false;
		if (r->pixels)
			draw_pixels(g, s, r);
		else
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
			    GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
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

/* The target's depth (as raster.c keeps it, for states without perspective_z) and stencil */
static bool read_depth(struct gpu *g, const struct gpu_target *t, float *depth, uint8_t *stencil)
{
	struct gl *gl = &g->gl;
	size_t n = (size_t)t->width * (size_t)t->height;
	uint32_t *both = malloc(n * 8);
	if (!both)
		return false;
	gl->BindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
	gl->ReadPixels(0, 0, t->width, t->height, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, both);
	for (size_t k = 0; k < n; k++) {
		memcpy(&depth[k], &both[2 * k], 4);
		if (stencil)
			stencil[k] = (uint8_t)both[2 * k + 1];
	}
	free(both);
	return true;
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
		states[k].unit[0].texture = &tx;
		states[k].unit[0].min_filter = RASTER_NEAREST_MIPMAP_NEAREST;
		raster_rave_stages(&states[k], k == 1 ? RASTER_TEX_MODULATE | RASTER_TEX_HIGHLIGHT :
						k == 2 ? RASTER_TEX_DECAL : 0);
	}
	states[1].unit[0].min_filter = RASTER_LINEAR_MIPMAP_NEAREST;
	states[1].unit[0].mag_filter = RASTER_LINEAR;
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
			v->color[0][0] = (float)((j + k) % 3) * 0.5f;
			v->color[0][1] = (float)(j % 2);
			v->color[0][2] = 0.25f * (float)k;
			v->color[0][3] = 0.4f + 0.3f * (float)j;
			v->tex[0][0] = (float)j * 0.7f * v->invw;
			v->tex[0][1] = (float)(2 - j) * 0.6f * v->invw;
			for (int i = 0; i < 3; i++) {
				v->color[1][i] = 0.1f;
				v->color[2][i] = 0.8f;
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

	struct raster_target cpu = { W, H, cpu_color, cpu_depth, NULL };
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
	struct program_key key[2];
	for (int k = 0; k < 2; k++) {
		memset(&key[k], 0, sizeof key[k]);
		key[k].perspective_z = (uint8_t)k;
	}
	if (!program_of(g, &key[0]) || !program_of(g, &key[1])) {
		snprintf(g->why, sizeof g->why, "OpenGL on %s can't build the shaders", gl->renderer);
		return false;
	}
	if (has_extension(g, "GL_EXT_texture_filter_anisotropic") ||
	    has_extension(g, "GL_ARB_texture_filter_anisotropic")) {
		gl->GetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &g->anisotropy);
		g->anisotropy = g->anisotropy > 8 ? 8 : g->anisotropy;
	}
	gl->GenSamplers(1, &g->bitmap_sampler);
	gl->SamplerParameteri(g->bitmap_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	gl->SamplerParameteri(g->bitmap_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	static const uint32_t white = 0xFFFFFFFFu;
	gl->GenTextures(1, &g->blank);
	gl->BindTexture(GL_TEXTURE_2D, g->blank);
	gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	gl->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, &white);
	gl->GenVertexArrays(1, &g->vao);
	gl->BindVertexArray(g->vao);
	gl->GenBuffers(1, &g->vbo);
	gl->BindBuffer(GL_ARRAY_BUFFER, g->vbo);
	static const struct { GLint size, offset; } attrib[8] = { { 4, 0 }, { 4, 4 }, { 4, 8 }, { 4, 12 }, { 1, 16 },
								  { 3, 17 }, { 3, 20 }, { 2, 23 } };
	for (GLuint k = 0; k < 8; k++) {
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
	g->blend_eq = GL_FUNC_ADD;
	gl->BlendColor(0, 0, 0, 0);
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
		for (unsigned k = 0; k < g->nprograms; k++)
			gl->DeleteProgram(g->programs[k].id);
		for (GLuint *s = &g->sampler[0][0][0][0]; s < &g->sampler[0][0][0][0] + sizeof g->sampler / sizeof *s; s++)
			if (*s)
				gl->DeleteSamplers(1, s);
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
	free(g->programs);
}

/* ---- the thread: jobs -------------------------------------------------------------------- */

static const char *const job_names[] = { "none", "target", "free", "copy", "draw", "read", "read depth", "quit" };

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
		fprintf(stderr, "gpu: failing job %lu (BLUE_GPU_FAIL)\n", g->jobs);
		g->failed = true;
		return;
	}
	bool ok = true;
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
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
	case JOB_READ_DEPTH:
		ok = read_depth(g, g->target, g->depth_out, g->stencil_out);
		break;
	default:
		break;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	uint64_t ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000u + (uint64_t)t1.tv_nsec - (uint64_t)t0.tv_nsec;
	if (g->job == JOB_DRAW)
		atomic_fetch_add(&g->draw_ns, ns);
	else if (g->job == JOB_READ)
		atomic_fetch_add(&g->read_ns, ns);
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
	g->debug = getenv("BLUE_GPU_DEBUG") != NULL;
	const char *fail = getenv("BLUE_GPU_FAIL");
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

void gpu_times(struct gpu *g, uint64_t *draw_ns, uint64_t *read_ns, uint64_t *uploads, uint64_t *upload_bytes)
{
	*draw_ns = *read_ns = *uploads = *upload_bytes = 0;
	if (!g)
		return;
	*draw_ns = atomic_load(&g->draw_ns);
	*read_ns = atomic_load(&g->read_ns);
	*uploads = atomic_load(&g->uploads);
	*upload_bytes = atomic_load(&g->upload_bytes);
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

bool gpu_read_depth(struct gpu *g, struct gpu_target *t, float *depth, uint8_t *stencil)
{
	if (!usable(g) || !t)
		return false;
	g->target = t;
	g->depth_out = depth;
	g->stencil_out = stencil;
	post(g, JOB_READ_DEPTH);
	wait_idle(g);
	return g->job_ok;
}
