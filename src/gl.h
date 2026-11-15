/*
 * OpenGL 3.3 core, loaded at run time (no build dependency, as with SDL2):
 * an off-screen context of our own, made with CGL on macOS and with EGL
 * (no window system needed) elsewhere, and the GL functions the 3D
 * engine's GPU backend uses. Only the parts of the ABIs we use are
 * declared here.
 */
#ifndef SYSHM_GL_H
#define SYSHM_GL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t GLenum;
typedef uint32_t GLuint;
typedef int32_t GLint;
typedef int32_t GLsizei;
typedef uint32_t GLbitfield;
typedef uint8_t GLboolean;
typedef uint8_t GLubyte;
typedef float GLfloat;
typedef double GLdouble;
typedef char GLchar;
typedef intptr_t GLsizeiptr;
typedef intptr_t GLintptr;
typedef uint64_t GLuint64;
typedef struct __GLsync *GLsync;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NONE 0
#define GL_NO_ERROR 0
#define GL_OUT_OF_MEMORY 0x0505

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_MAJOR_VERSION 0x821B
#define GL_MINOR_VERSION 0x821C
#define GL_NUM_EXTENSIONS 0x821D
#define GL_CONTEXT_PROFILE_MASK 0x9126
#define GL_CONTEXT_CORE_PROFILE_BIT 0x00000001
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAX_VIEWPORT_DIMS 0x0D3A
#define GL_MAX_RENDERBUFFER_SIZE 0x84E8
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF   /* (GL_EXT_texture_filter_anisotropic) */

#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_DITHER 0x0BD0
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_MULTISAMPLE 0x809D
#define GL_DEPTH_CLAMP 0x864F
#define GL_FRAMEBUFFER_SRGB 0x8DB9

#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_COLOR_BUFFER_BIT 0x00004000

#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207

#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE 0x0308
#define GL_CONSTANT_COLOR 0x8001
#define GL_ONE_MINUS_CONSTANT_COLOR 0x8002
#define GL_CONSTANT_ALPHA 0x8003
#define GL_ONE_MINUS_CONSTANT_ALPHA 0x8004
#define GL_FUNC_ADD 0x8006
#define GL_MIN 0x8007
#define GL_MAX 0x8008
#define GL_FUNC_SUBTRACT 0x800A
#define GL_FUNC_REVERSE_SUBTRACT 0x800B

#define GL_TRIANGLES 0x0004

#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#define GL_DEPTH_COMPONENT 0x1902
#define GL_RGBA 0x1908
#define GL_BGRA 0x80E1
#define GL_RGBA8 0x8058
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_COMPONENT32F 0x8CAC

#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_PACK_ALIGNMENT 0x0D05

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE       /* (GL_EXT_texture_filter_anisotropic) */
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F

#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5

#define GL_ARRAY_BUFFER 0x8892
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_STREAM_DRAW 0x88E0
#define GL_STREAM_READ 0x88E1
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_MAP_READ_BIT 0x0001
#define GL_MAP_WRITE_BIT 0x0002
#define GL_MAP_INVALIDATE_RANGE_BIT 0x0004
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x0008
#define GL_MAP_UNSYNCHRONIZED_BIT 0x0020

#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84

#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#define GL_ALREADY_SIGNALED 0x911A
#define GL_TIMEOUT_EXPIRED 0x911B
#define GL_CONDITION_SATISFIED 0x911C
#define GL_WAIT_FAILED 0x911D
#define GL_TIMEOUT_IGNORED 0xFFFFFFFFFFFFFFFFull

/* The functions loaded: X(return type, name without "gl", parameters) */
#define GL_FUNCTIONS(X) \
	X(GLenum, GetError, (void)) \
	X(const GLubyte *, GetString, (GLenum name)) \
	X(const GLubyte *, GetStringi, (GLenum name, GLuint index)) \
	X(void, GetIntegerv, (GLenum pname, GLint *data)) \
	X(void, GetFloatv, (GLenum pname, GLfloat *data)) \
	X(void, Enable, (GLenum cap)) \
	X(void, Disable, (GLenum cap)) \
	X(void, Viewport, (GLint x, GLint y, GLsizei width, GLsizei height)) \
	X(void, Scissor, (GLint x, GLint y, GLsizei width, GLsizei height)) \
	X(void, ClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) \
	X(void, ClearDepth, (GLdouble depth)) \
	X(void, Clear, (GLbitfield mask)) \
	X(void, ColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a)) \
	X(void, DepthMask, (GLboolean flag)) \
	X(void, DepthFunc, (GLenum func)) \
	X(void, BlendFunc, (GLenum sfactor, GLenum dfactor)) \
	X(void, BlendFuncSeparate, (GLenum srgb, GLenum drgb, GLenum salpha, GLenum dalpha)) \
	X(void, BlendEquation, (GLenum mode)) \
	X(void, BlendColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a)) \
	X(void, PixelStorei, (GLenum pname, GLint param)) \
	X(void, ReadPixels, (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, \
			     void *pixels)) \
	X(void, Finish, (void)) \
	X(void, Flush, (void)) \
	X(void, GenTextures, (GLsizei n, GLuint *textures)) \
	X(void, DeleteTextures, (GLsizei n, const GLuint *textures)) \
	X(void, BindTexture, (GLenum target, GLuint texture)) \
	X(void, ActiveTexture, (GLenum texture)) \
	X(void, TexImage2D, (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, \
			     GLint border, GLenum format, GLenum type, const void *pixels)) \
	X(void, TexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, \
				GLenum format, GLenum type, const void *pixels)) \
	X(void, TexParameteri, (GLenum target, GLenum pname, GLint param)) \
	X(void, TexParameterf, (GLenum target, GLenum pname, GLfloat param)) \
	X(void, GenSamplers, (GLsizei n, GLuint *samplers)) \
	X(void, DeleteSamplers, (GLsizei n, const GLuint *samplers)) \
	X(void, BindSampler, (GLuint unit, GLuint sampler)) \
	X(void, SamplerParameteri, (GLuint sampler, GLenum pname, GLint param)) \
	X(void, SamplerParameterf, (GLuint sampler, GLenum pname, GLfloat param)) \
	X(void, GenFramebuffers, (GLsizei n, GLuint *framebuffers)) \
	X(void, DeleteFramebuffers, (GLsizei n, const GLuint *framebuffers)) \
	X(void, BindFramebuffer, (GLenum target, GLuint framebuffer)) \
	X(void, FramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum rbtarget, GLuint renderbuffer)) \
	X(void, FramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, \
				       GLint level)) \
	X(GLenum, CheckFramebufferStatus, (GLenum target)) \
	X(void, BlitFramebuffer, (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, \
				  GLint dy1, GLbitfield mask, GLenum filter)) \
	X(void, ReadBuffer, (GLenum mode)) \
	X(void, DrawBuffer, (GLenum mode)) \
	X(void, GenRenderbuffers, (GLsizei n, GLuint *renderbuffers)) \
	X(void, DeleteRenderbuffers, (GLsizei n, const GLuint *renderbuffers)) \
	X(void, BindRenderbuffer, (GLenum target, GLuint renderbuffer)) \
	X(void, RenderbufferStorage, (GLenum target, GLenum internalformat, GLsizei width, GLsizei height)) \
	X(void, GenBuffers, (GLsizei n, GLuint *buffers)) \
	X(void, DeleteBuffers, (GLsizei n, const GLuint *buffers)) \
	X(void, BindBuffer, (GLenum target, GLuint buffer)) \
	X(void, BufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage)) \
	X(void, BufferSubData, (GLenum target, GLintptr offset, GLsizeiptr size, const void *data)) \
	X(void *, MapBufferRange, (GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access)) \
	X(GLboolean, UnmapBuffer, (GLenum target)) \
	X(void, GenVertexArrays, (GLsizei n, GLuint *arrays)) \
	X(void, DeleteVertexArrays, (GLsizei n, const GLuint *arrays)) \
	X(void, BindVertexArray, (GLuint array)) \
	X(void, EnableVertexAttribArray, (GLuint index)) \
	X(void, VertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, \
				      const void *pointer)) \
	X(GLuint, CreateShader, (GLenum type)) \
	X(void, ShaderSource, (GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length)) \
	X(void, CompileShader, (GLuint shader)) \
	X(void, GetShaderiv, (GLuint shader, GLenum pname, GLint *params)) \
	X(void, GetShaderInfoLog, (GLuint shader, GLsizei size, GLsizei *length, GLchar *log)) \
	X(void, DeleteShader, (GLuint shader)) \
	X(GLuint, CreateProgram, (void)) \
	X(void, AttachShader, (GLuint program, GLuint shader)) \
	X(void, LinkProgram, (GLuint program)) \
	X(void, GetProgramiv, (GLuint program, GLenum pname, GLint *params)) \
	X(void, GetProgramInfoLog, (GLuint program, GLsizei size, GLsizei *length, GLchar *log)) \
	X(void, DeleteProgram, (GLuint program)) \
	X(void, UseProgram, (GLuint program)) \
	X(void, BindAttribLocation, (GLuint program, GLuint index, const GLchar *name)) \
	X(void, BindFragDataLocation, (GLuint program, GLuint color, const GLchar *name)) \
	X(GLint, GetUniformLocation, (GLuint program, const GLchar *name)) \
	X(void, Uniform1i, (GLint location, GLint v0)) \
	X(void, Uniform1f, (GLint location, GLfloat v0)) \
	X(void, Uniform2f, (GLint location, GLfloat v0, GLfloat v1)) \
	X(void, Uniform3f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2)) \
	X(void, Uniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)) \
	X(void, Uniform1fv, (GLint location, GLsizei count, const GLfloat *value)) \
	X(void, Uniform4fv, (GLint location, GLsizei count, const GLfloat *value)) \
	X(void, DrawArrays, (GLenum mode, GLint first, GLsizei count)) \
	X(GLsync, FenceSync, (GLenum condition, GLbitfield flags)) \
	X(GLenum, ClientWaitSync, (GLsync sync, GLbitfield flags, GLuint64 timeout)) \
	X(void, DeleteSync, (GLsync sync))

struct gl {
	void *priv;                     /* the platform's (gl.c's) */
	char renderer[128];             /* GL_RENDERER */
	char version[128];              /* GL_VERSION */
	int major, minor;               /* GL_MAJOR_VERSION, GL_MINOR_VERSION */
#define GL_MEMBER(ret, name, params) ret (*name) params;
	GL_FUNCTIONS(GL_MEMBER)
#undef GL_MEMBER
};

/*
 * Makes an OpenGL 3.3 (or later) core profile context that renders off
 * screen only, current on the calling thread, and loads every function in
 * GL_FUNCTIONS. A hardware renderer is preferred; with `allow_software`, a
 * software one is taken if there's no other. Returns false with the reason
 * in `why` (and nothing left open) if there's no such context.
 */
bool gl_open(struct gl *gl, bool allow_software, char *why, size_t why_size);
void gl_close(struct gl *gl);

/* Whether a GL_RENDERER string names a software rasterizer (llvmpipe, Apple's...) */
bool gl_software_renderer(const char *renderer);

#endif
