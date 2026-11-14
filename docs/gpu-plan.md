# Plan: drawing the 3D engine with OpenGL 3.3

The guest tools' RAVE engine (`src/rave.c`) draws everything with the
software rasterizer in `src/raster.c`. This plan adds a second backend that
draws the same recorded commands with the host's GPU through OpenGL 3.3
core, used automatically when the host has hardware acceleration, and keeps
the software rasterizer as the reference and the fallback.

## Status

Built (milestones 1 to 5): `src/gl.c` (CGL, EGL), `src/gpu.c`, the seam in
`rave.c`, the wiring in `nk.c`, `tests/test_gpu.c`, CI on Mesa.

- Tests: on an Apple M5 (OpenGL 4.1 Metal) and Mesa's llvmpipe (in a
  container, as CI runs it), the exact tests (coverage, depth in both
  modes, texture orientation, clears, points, lines, bitmaps, copies)
  match `raster.c` pixel for pixel; `test_raster.c`'s random scene differs
  by more than 3 in 17 (M5) or 3 (llvmpipe) pixels of 3072, all at
  triangle edges; every blend factor works as source and destination on
  both.
- One feature at a time against `raster.c` (random scenes): 0.1-0.4% of
  pixels differ (edges), but for mipmapped textures under strong
  perspective (the GPU's per-pixel levels; without perspective the levels
  are the same) and z outside 0..1 (clamped).
- Speed, whole frames read back, textured, fogged, a fifth blended, on the
  M5: 640x480 2.4x the software rasterizer (366 fps against 153),
  1024x768 4.6x, 1280x1024 5.1x (6.6x with 10,000 triangles), and the
  software rasterizer's two helper threads are free.

Milestone 6 (performance) is still to do, as is the in-game comparison
with Bugdom.

## Decisions

- **API:** OpenGL 3.3 core profile (4.1 core on macOS, which accepts
  `#version 330 core` shaders). One backend for Linux and macOS, plain C,
  loaded at run time with `dlopen` like SDL2, so no new build dependency.
- **Off-screen, on its own thread:** the GPU backend renders into
  framebuffer objects in a context of its own (CGL on macOS, EGL without a
  window on Linux), owned by a dedicated thread. It never touches the
  window's SDL renderer or its context.
- **Pixels go back to the guest:** at the end of each frame the GPU's
  image is read back into the context's host buffer, and `present()` copies
  it into guest memory exactly as it does now.
- **When it's used:** automatically in real-time runs (`--window`,
  `--realtime`) when the host has hardware-accelerated OpenGL 3.3.
  Instruction-time runs keep the software rasterizer, so they stay exactly
  reproducible on every host. `--no-gpu` turns it off; `SYSHM_GPU=1`
  forces it on in any run, accepting software OpenGL too (for tests and
  comparisons); `SYSHM_GPU=0` is the same as `--no-gpu`.
- **Invisible to the guest except for the pixels:** the engine fragment,
  its gestalt answers (ATI vendor, engine 5, features, texture memory) and
  every method behave the same with either backend.
- **Fallback:** if OpenGL can't be loaded, isn't 3.3, is a software
  renderer, fails its self-test, or fails later, drawing continues in
  software with a one-line message.

## What we build on

- `rave.c` turns RAVE calls into `raster_cmd`s (triangles, lines, points,
  bitmaps, clears) with a `raster_state` index each, in batches of 512.
  A full batch goes to `raster_start()` (drawn in the background on the
  raster thread pool) while the next is recorded; `draw_frame()` sorts the
  deferred transparent triangles (`kQATag_ZSortedHint`), appends them and
  draws the rest; `forget_frame()` waits.
- Each context has a host `raster_target` (`color` as `0xAARRGGBB`, `depth`
  as floats). `renderStart` clears it or copies another context's;
  `renderEnd`/`swapBuffers` draws the frame and `present()` converts the
  colors into the device's pixels (big-endian 16 or 32 bits) in guest
  memory, clipped to the context's clip region's bounding box.
- Textures and bitmaps are converted to `0xAARRGGBB` when the guest
  creates them (`image_new`) and never change afterwards (accessTexture is
  not supported). A deleted image whose texture recorded drawing still uses
  is freed later (`doomed`).
- RAVE calls are host calls on the emulation thread, which is also the
  thread that drives the SDL window (`display_frame()` in `nk.c`).

## Architecture

### The seam in `rave.c`

`rave.c` keeps recording exactly as now. A context gets a backend when it
is created, and five operations go through it:

| Operation | Software (as now) | GPU |
|---|---|---|
| draw a batch in the background | `raster_start()` | `gpu_start()` |
| wait for it | `raster_wait()` | `gpu_wait()` |
| start a frame from another context | `memcpy` color and depth | `gpu_copy()` (blit) |
| finish a frame for `present()` | nothing: pixels are in `target.color` | `gpu_read()` into `target.color` |
| free a context, a texture | `free()` | `gpu_target_free()`, `gpu_texture_free()` |

`renderStart`'s full clear becomes a `RASTER_CMD_CLEAR` at the start of
the frame's first batch instead of a direct `raster_clear()`, so both
backends clear the same way. So that the software pixels don't change at
all: the clear doesn't count toward that batch's 512 commands, and an
aborted frame still does the clear if its batch hadn't started (another
context's frame can start from this one's buffers). Checked by driving the
old and new `rave.c` with the same 20 random streams of RAVE calls (300
frames, about half a million triangles each): byte for byte the same.

Because the GPU backend fills the same `target.color` before `present()`,
everything downstream stays as it is: memory-device contexts, 16- and
32-bit conversion, clipping, the window's change detection.

### New files

- `src/gpu.h`: the backend's interface (below).
- `src/gl.h`, `src/gl.c`: loading OpenGL and making the context, with CGL
  on macOS and EGL elsewhere. Like `display.c` with SDL2, they declare the
  parts of the GL 3.3, EGL and CGL ABIs they use themselves, so building
  needs no GL headers. Both platforms' code is built on every host and
  picked at run time, so `-Werror` builds check both.
- `src/gpu.c`: the thread, the acceptance checks and self-test, the
  shaders, and drawing.
- `tests/test_gpu.c`: the backend against `raster.c`.

```c
struct gpu;
struct gpu_target;

/* Starts the GPU thread, which opens and checks a context in the background */
struct gpu *gpu_open(bool allow_software);
/* Waits for that; false if there's no usable OpenGL (*why says what was missing) */
bool gpu_ready(struct gpu *g, const char **why);
const char *gpu_renderer(const struct gpu *g);  /* "OpenGL 4.1 Metal - 89.4, Apple M1 Max" */
bool gpu_failed(const struct gpu *g);           /* failed since it was ready: draw in software */
void gpu_close(struct gpu *g);

struct gpu_target *gpu_target_new(struct gpu *g, int width, int height);
void gpu_target_free(struct gpu *g, struct gpu_target *t);
void gpu_copy(struct gpu *g, struct gpu_target *to, struct gpu_target *from);

/* As raster_start() and raster_wait(): the arrays stay as they are until gpu_wait() */
void gpu_start(struct gpu *g, struct gpu_target *t, const struct raster_state *states,
	       const struct raster_cmd *cmds, unsigned n);
void gpu_wait(struct gpu *g);

/* The target's pixels into `color` (0xAARRGGBB, rows top to bottom); false if the GPU failed */
bool gpu_read(struct gpu *g, struct gpu_target *t, uint32_t *color);

/* Forgets a texture's GPU copy (before raster.c's copy is freed) */
void gpu_texture_free(struct gpu *g, struct raster_texture *tex);
```

### The GPU thread

All OpenGL calls happen on one thread that owns the context:

- On Linux, SDL's own OpenGL renderer keeps track of which context is
  current itself, and GLVND won't make an EGL context current on a thread
  where a GLX one is. Our own thread avoids both problems, and works the
  same whatever renderer SDL picked (usually Metal on macOS and OpenGL on
  Linux).
- It overlaps with emulation as the raster threads do now: handing over a
  batch means copying its vertices and issuing draw calls, after which the
  GPU works while the guest records the next batch.

Hand-over follows the raster pool's rules: one job at a time, `gpu_*`
calls wait for the previous job before posting theirs, with a mutex and
condition variables. A job is "done" when its OpenGL calls have been made,
not when the GPU has finished them; only `gpu_read()` waits for the GPU.
Job kinds: create or free a target, draw a batch, copy, read back, free
textures.

Initialization runs on that thread as soon as `gpu_open()` is called at
boot, so the first 3D frame doesn't wait for it (booting to a game takes
seconds; loading OpenGL, compiling shaders and the self-test take a
fraction of one). `gpu_ready()` blocks only if it hasn't finished. The
backend prints nothing itself; `rave.c` asks `gpu_ready()` when the guest
makes its first context and says which backend draws:

```
3d: drawing with OpenGL 4.1 Metal - 91.7 (Apple M5)
3d: drawing in software (OpenGL renderer llvmpipe (LLVM 20.1.2, 128 bits) is software, not hardware)
```

### Loading OpenGL without a build dependency

macOS:

1. `dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL")`.
2. `CGLChoosePixelFormat` with `kCGLPFAOpenGLProfile` =
   `kCGLOGLPVersion_3_2_Core` (which gives 4.1), `kCGLPFAAccelerated`,
   `kCGLPFANoRecovery` (no fallback to Apple's software renderer),
   `kCGLPFAAllowOfflineRenderers` and
   `kCGLPFASupportsAutomaticGraphicsSwitching` (an off-screen context
   doesn't power up a discrete GPU on dual-GPU Macs).
3. `CGLCreateContext`, `CGLSetCurrentContext` on the GPU thread; GL
   functions by `dlsym` on the framework.

Linux:

1. `dlopen("libEGL.so.1")`.
2. A display without a window system, trying in order:
   `EGL_PLATFORM_SURFACELESS_MESA` (Mesa, through a render node),
   `EGL_PLATFORM_DEVICE_EXT` over `eglQueryDevicesEXT` (NVIDIA's driver),
   then `eglGetDisplay(EGL_DEFAULT_DISPLAY)`. This works the same under
   X11, Wayland, or with no display server at all.
3. `eglBindAPI(EGL_OPENGL_API)`, a context with
   `EGL_CONTEXT_MAJOR_VERSION` 3, `MINOR` 3 and the core profile;
   `eglMakeCurrent` with no surface (`EGL_KHR_surfaceless_context`), or a
   1x1 pbuffer if that isn't supported.
4. GL functions by `eglGetProcAddress` (core functions included with EGL
   1.5 or `EGL_KHR_get_all_proc_addresses`), else `dlsym` on
   `libOpenGL.so.0` or `libGL.so.1`.

Every symbol is looked up up front; a missing one means no GPU.

### Deciding the host supports acceleration

`nk_boot()` starts the GPU when:

```
--no-gpu or SYSHM_GPU=0   → never
SYSHM_GPU=1               → in any run, software OpenGL accepted
otherwise                 → in real-time runs, hardware OpenGL only
```

The GPU thread then accepts the context only if all of these hold:

1. The context was created with OpenGL 3.3 core or later (`GL_VERSION`,
   `GL_MAJOR_VERSION`/`GL_MINOR_VERSION`).
2. It's a hardware renderer: `GL_RENDERER` doesn't name a software one
   (`llvmpipe`, `softpipe`, `swrast`, `Software Rasterizer`,
   `Apple Software Renderer`, `SwiftShader`; zink on lavapipe shows as
   `llvmpipe` too). Virtual GPUs that pass drawing to the host's
   (`virgl`, VMware's `SVGA3D`, Parallels) count as hardware. Skipped
   with `SYSHM_GPU=1`.
3. Limits: `GL_MAX_TEXTURE_SIZE` and `GL_MAX_RENDERBUFFER_SIZE` of at
   least 4096 (the largest context and image RAVE allows), and 32-bit
   float depth renderbuffers that make complete framebuffers.
4. Both shader programs compile and link.
5. **Self-test:** a small built-in scene (depth testing, a mipmapped
   bilinear texture, modulate and highlight, linear fog, each blend mode,
   alpha test, a bitmap, a clear) drawn by both backends matches within
   the tolerances of the tests below. This catches broken drivers before
   the guest sees them.

## Rendering

Everything `raster.c` does, mapped onto OpenGL. The aim is pixels that
match the software rasterizer's except for rounding at triangle edges
and in filtering (see Known differences).

### Coordinates

- Vertices arrive in pixels, x right and y down, with pixel centers at
  half-integers and no clipping in z, as in `raster.c`.
- The vertex shader maps them to clip space with w = 1:
  `x_ndc = 2x/W - 1` and `y_ndc = 2y/H - 1`. Image row 0 then lands in
  OpenGL's row 0, so `glReadPixels` returns rows top first and scissor,
  clear and read rectangles use image coordinates unchanged.
- With w = 1 the GPU interpolates every value linearly in screen space,
  which is what `raster.c` does (it interpolates u/w, v/w and 1/w and
  divides per pixel). Inputs are declared `noperspective` regardless.
- `GL_DEPTH_CLAMP` is on, so nothing is clipped against near and far.

### Vertex format

One vertex per triangle corner, not indexed, since commands carry their
own three vertices: position (2 floats), z, 1/w, rgba (4), u/w and v/w
(2), kd (3) and ks (3), 16 floats. The GPU thread builds them from the
commands. A Bugdom frame of a few thousand triangles is a few hundred
kilobytes.

### Depth

Two program variants, one per depth mode:

- **z** (`perspective_z` off): depth is z, through `gl_Position.z`, so
  the GPU can test it early. Depth functions map directly (`glDepthFunc`).
  Clears to `raster_far()` (1).
- **Perspective z** (tests -1/w): the fragment shader writes
  `gl_FragDepth = invw / (1 + invw)` from the interpolated 1/w. That's
  monotonic in 1/w, and like "reversed z" it keeps float precision for
  distant geometry. The comparison is flipped (LT becomes GT, LE becomes
  GE, and so on), and a clear to `raster_far()` (0) clears to 0.

The depth buffer is `GL_DEPTH_COMPONENT32F`. `RASTER_Z_NONE` disables
the depth test, which also stops depth writes as in `raster.c`;
`z_write` is `glDepthMask`.

### Textures

- Uploaded by the GPU thread the first time a batch uses them, as
  `GL_RGBA8` from `GL_BGRA` and `GL_UNSIGNED_INT_8_8_8_8_REV`, which reads
  host `0xAARRGGBB` words with the right channels. Rows go in the same
  order as `raster_texture`'s, so texture coordinates mean the same thing.
- `raster_texture` gets a `gpu_name` field: 0 until uploaded, used only by
  the GPU thread, and by `rave.c` after `gpu_wait()` to free it. Texels
  stay in host memory, for the software fallback.
- Mipmaps: `GL_TEXTURE_MAX_LEVEL` is `levels - 1`. A level whose size
  isn't half the previous one (rounded down, at least 1) would make the
  texture incomplete, so the GPU copy keeps only the levels before it.
- **The GPU picks mipmap levels per pixel** (decided: better looking than
  `raster.c`'s one level per triangle, and real-time only). RAVE's texture
  filter tag chooses the quality: Fast (0) is nearest with the nearest
  level, Mid (1) bilinear with the nearest level, Best (2, `trilinear` in
  `raster_state`) trilinear, with anisotropic filtering where the driver
  has `GL_EXT_texture_filter_anisotropic`. Sampler objects cover the
  combinations of filter and clamp or repeat in u and v; the shader
  samples with `texture()`, before any `discard`, so derivatives are
  defined.

### Fragment shader

In `fragment()`'s order:

1. Texture: u = (u/w) / (1/w), and likewise v (1/w = 0 gives the same
   1e30 as `raster.c`). Decal blends the texel over the vertex color by
   texel alpha and keeps the vertex alpha; otherwise alpha is texel times
   vertex. Then modulate (times kd, clamped) and highlight (plus ks,
   clamped).
2. Fog by w: alpha fog (the factor is alpha, and alpha becomes 1), linear
   between start and end, exp and exp², with `raster.c`'s density
   scaling, mixed toward the fog color.
3. Alpha test against the reference: `discard` if it fails.
4. Fully transparent fragments: `discard` when alpha <= 0 and either the
   blend is interpolate or the color adds nothing, as `raster.c` skips
   them, depth included.

The per-state settings (texture op, whether textured, fog mode and
parameters, alpha test, bitmap mode) are uniforms, set when the state
changes.

### Blending

Fixed-function, per state:

| RAVE blend | Color | Alpha |
|---|---|---|
| premultiply | `ONE, ONE_MINUS_SRC_ALPHA` | `ONE, ONE_MINUS_SRC_ALPHA` |
| interpolate | `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` | `ONE, ONE_MINUS_SRC_ALPHA` |
| OpenGL factors | the two factors | the same factors |

The alpha column gives `1 - (1 - a)(1 - d)`, which is what `raster.c`
stores. `raster.c` blends only when alpha < 1, but with alpha at 1 these
equations give the same result. `SRC_ALPHA_SATURATE` maps directly.
Output is clamped by the 8-bit framebuffer as `pack()` clamps.

### Clears, clipping and copies

- A state's clip rectangle (the whole target in `state_of()` today) is
  `glScissor`.
- `RASTER_CMD_CLEAR`: scissor to the rectangle within the clip, color
  and depth masks on, `glClearColor` from the ARGB value, `glClearDepth`
  from z mapped for the state's depth mode, `glClear` for whichever of
  color and depth the command asks for. Clears happen in command order
  between draws.
- `gpu_copy()`: `glBlitFramebuffer` of color and depth, `GL_NEAREST`.
  Both targets are the same size, as `render_start()` already requires.

### Points, lines and bitmaps

These are made of quads that `raster.c`'s own arithmetic places on whole
pixels, so they come out exactly as in software:

- **Points:** one square per point, at the pixel bounds `plot()`
  computes, with the vertex's values flat across it.
- **Lines:** one square per step at `raster_line()`'s positions and
  interpolated values, as it stamps them. Lines are rare (wireframe), so a
  quad per step is cheap.
- **Bitmaps:** a quad from `floor(x + 0.5)`, `floor(y + 0.5)` at the
  image's size, in bitmap mode: `texelFetch` at the pixel's offset into
  the image, no texture op or fog, depth z (or 1/w = 1 in perspective z),
  and alpha-0 texels discarded.

### Batching and draw calls

For each job the GPU thread walks the commands in order, appends vertices
to one array, and cuts it into draw ranges wherever the state, the
program variant or the kind of primitive changes, or a clear comes in
between. It uploads the array once (`glBufferData` orphaning), then makes
one `glDrawArrays` per range. Primitive order is kept within and between
draw calls, so blending and depth see commands in order, as each
`raster.c` band does. The sorted transparent triangles are already in
order when they arrive.

### Reading back

`gpu_read()` posts a job that reads the whole target with `glReadPixels`
(`GL_BGRA`, `GL_UNSIGNED_INT_8_8_8_8_REV`, pack alignment 4) straight into
`target.color` (packed host words, so `0xAARRGGBB` as `raster.c` has them on
any host), then checks
`glGetError`. That waits for the GPU to finish the frame. It's 1.2 MB at
640x480 and 5 MB at 1280x1024, nearly free with Apple Silicon's shared
memory, and a fraction of a millisecond over PCIe. Reading only the
presented rectangle comes later (see Performance).

## Changes to existing code

- **`src/raster.h`:** `gpu` in `raster_texture` (the GPU's copy), and
  `trilinear` in `raster_state` (RAVE's Best filtering, which only the GPU
  uses).
- **`src/rave.h`, `src/rave.c`:**
  - `struct rave` gets `struct gpu *gpu` (not owned); `struct
    rave_context` gets `struct gpu_target *gpu` (NULL: software).
  - `context_new()`: once `gpu_ready()`, a GPU target as well as the host
    buffers (still needed for reading back into and for falling back).
  - `start_batch()`, `draw_frame()`, `forget_frame()`, `render_start()`,
    `free_context()`, `delete_image()`, `free_doomed()` and `rave_free()`
    go through the seam above; `renderEnd`/`swapBuffers` calls
    `gpu_read()` before `present()`.
  - `renderStart`'s clear as a command, for both backends.
  - On `gpu_failed()` or a failing `gpu_read()`: free every GPU target,
    set `r->gpu` to NULL, print once, and go on in software (that frame is
    lost; the next `renderStart` clears as usual).
  - The `SYSHM_RAVE_FPS` line names the backend.
- **`src/nk.h`, `src/nk.c`:**
  - `bool no_gpu` in `nk_boot_options`.
  - `struct machine` gets `struct gpu *gpu`, opened in `nk_boot()` before
    `start_machine()` by the rules above, given to the engine in
    `setup_machine()` after `rave_init()`, kept across restarts like the
    window and sound (`restart_machine()`), closed after
    `stop_machine()`.
  - The "3d engine" summary line says which backend drew.
- **`src/main.c`:** `--no-gpu`, and its line in the usage text.
- **`Makefile`:** `src/gpu.c` in `LIB_SRC`. No new libraries: `-ldl` and
  `-lpthread` are already there.
- **`tests/`:** `test_gpu.c`, its entry in `test.h` and `tests/main.c`;
  `test_rave.c` runs its frames with each backend there is.
- **`.github/workflows/ci.yml`:** on Linux, install Mesa's EGL and
  software drivers (`libegl1 libegl-mesa0 libgl1-mesa-dri`) and run the
  tests with `SYSHM_TEST_GPU=1`, so the backend is exercised on llvmpipe
  on every push and a missing context fails instead of skipping. macOS
  runners run the GPU tests if they can open a context and skip them
  otherwise.
- **`README.md`:** the 3D engine paragraph (drawn with OpenGL when
  available, and when it isn't), `--no-gpu` and `SYSHM_GPU`, Next steps,
  Layout, Build and test.

## Known differences from the software renderer

Real-time runs only, unless `SYSHM_GPU=1`:

1. **Triangle edges:** GPUs snap vertices to a subpixel grid (typically
   1/256 pixel) and evaluate edges in fixed point, so pixels whose centers
   lie on or very near an edge can go to the other triangle. Shared edges
   stay watertight.
2. **Interpolation and arithmetic:** colors, texture coordinates and fog
   can differ by a unit in the last 8-bit place.
3. **Bilinear filtering:** GPUs weigh texels with a few bits of fraction
   (often 8), `raster.c` with float.
4. **Transparent fragments that add color:** with the premultiply or
   OpenGL-factor blends, a fragment with alpha 0 but some color is drawn
   in both backends, but `raster.c` doesn't write its depth and the GPU
   does when depth writes are on (it can't skip the write per fragment).
   QuickDraw 3D turns depth writes off for transparency, so this should be
   rare.
5. **z outside 0..1** is clamped by the depth buffer; `raster.c` compares
   any value.
6. **Mipmapping:** the GPU picks levels per pixel (trilinear with Best
   filtering), `raster.c` one level per triangle; and mipmap chains with
   odd sizes are cut short on the GPU (see Textures).
7. **Perspective z precision:** `invw / (1 + invw)` in float, not -1/w,
   so near-coincident surfaces can resolve differently.

## Errors and fallback

- During initialization: any failure leaves `gpu_ready()` false with a
  reason, printed once; every context draws in software.
- At run time: `glGetError` after every job, and framebuffer completeness
  when a target is made (`SYSHM_GPU_DEBUG=1` also logs each job and
  shader logs). An error marks the GPU failed; `rave.c` switches to
  software as above. The context isn't used again for the rest of the
  run, restarts included.
- `SYSHM_GPU_FAIL=N` (testing only) fails the Nth job, so the fallback
  path can be tested.
- The JIT's fastmem `SIGSEGV` handler passes on faults that aren't its
  own, but a driver that installed its own handler after it could swallow
  them. (Not done yet:) after `gpu_ready()`, `nk.c` should check the
  `SIGSEGV` handler is still the JIT's and say so if not (with the same
  advice as for gdb: `SYSHM_JIT_FASTMEM=0`).

## Testing

`tests/test_gpu.c`, run when a context opens (`allow_software`, so Mesa's
llvmpipe counts) and skipped otherwise, except that `SYSHM_TEST_GPU=1`
makes no context a failure:

- **Exact**, using whole-pixel geometry where GPUs agree with `raster.c`:
  - coverage: two triangles sharing an edge, added up, cover a square
    exactly once (as `test_coverage`)
  - depth: the nearer triangle wins in both orders, in both depth modes
  - texture orientation with nearest sampling (as `test_texture`)
  - clears of a rectangle within a clip; reading back top to bottom
  - points, lines and bitmaps against `raster.c`, pixel for pixel
  - copying one target into another
- **Within tolerance:** `test_threads`' 400 random commands across six
  states (blend modes, fog, filtering, texture ops, clips, clears, lines)
  drawn by both backends. At least 97% of pixels within 3 per channel, and
  every pixel more than 3 off next to a triangle edge in the software
  image. Thresholds to be tuned on llvmpipe and real GPUs, then fixed.
- **`test_rave`** runs both frames (texture orientation, 1,500 triangles
  over several batches) with each backend.
- **Fallback:** with `SYSHM_GPU_FAIL`, a frame after the failure comes out
  of the software backend.

By hand, with Bugdom:

- `SYSHM_RAVE_FPS=1` with and without `--no-gpu`, at 640x480 and at
  1024x768 or more (the guest tools' syshm Enhancements lets the window set the size).
- The same scripted instruction-time run with and without `SYSHM_GPU=1`,
  comparing screenshots (for example with ImageMagick's
  `compare -metric AE`): differences should be at edges only.
- Fog, water and other transparency, the HUD (bitmaps), menus, changing
  the resolution (contexts are recreated), Restart, closing the window.

## Milestones

Each one leaves the tree working, with `make test` passing on Linux and
macOS.

0. **Baseline.** `SYSHM_RAVE_FPS=1` numbers for Bugdom in software at two
   resolutions, and screenshots from a scripted instruction-time run, to
   compare against throughout.
1. **The seam, no change in behaviour.** Backend calls in `rave.c` with
   only software behind them; `renderStart`'s clear as a command;
   `raster_triangle_level()` exported; `gpu_name` added. Done when the
   baseline screenshots come out identical, byte for byte.
2. **Loading, context, thread, acceptance.** `gpu.c` opens and checks a
   context on macOS and Linux and prints the result; `--no-gpu`,
   `SYSHM_GPU`; nothing drawn on the GPU yet. Done when the message is
   right on an Apple Silicon Mac, on Linux with a GPU, on Linux with only
   llvmpipe (rejected without `SYSHM_GPU=1`), and over SSH with no
   display.
3. **Drawing.** In this order, each with its tests: targets, clears and
   reading back; untextured triangles and depth (both modes); textures,
   samplers and mipmaps; texture ops; fog; alpha test and discards;
   blending; bitmaps, points and lines; copies; batching across jobs. Then
   the self-test. Done when `test_gpu` and `test_rave` pass on llvmpipe
   and on hardware.
4. **Integration.** `nk.c` wiring, restarts, run-time fallback, the
   summary line. Done when Bugdom plays on the GPU, looks like the
   software renderer, runs at least as fast, survives Restart, and falls
   back cleanly with `SYSHM_GPU_FAIL`.
5. **CI and documentation.** Mesa in the Linux jobs with
   `SYSHM_TEST_GPU=1`; README and usage text.
6. **Performance**, measured against the baseline (below).

Rough size: `gpu.c` around 1,500 lines including the ABI declarations and
shaders, about 150 lines changed in `rave.c`, about 300 lines of tests.

## Performance, once it works

In order of expected value, each kept only if it measures faster:

- Read back only the rectangle `present()` will copy (the frame's rect
  clipped to the clip region and device), not the whole target.
- Convert to the guest's pixel format on the GPU (16-bit 1-5-5-5 or
  32-bit, big-endian) and read rows straight into guest memory, skipping
  `present()`'s per-pixel loop on the emulation thread.
- Larger batches for the GPU backend (fewer hand-overs than 512 commands
  each).
- Stream vertices through a ring buffer with unsynchronized mapping
  instead of orphaning.
- Skip reading back a frame that drew nothing since the last one.

The window shows at most 60 frames a second (`display_frame()`), so past
that a faster engine only gives the guest more time.

## Risks and open questions

- **OpenGL on macOS** has been deprecated since macOS 10.14 but still
  ships. If it goes, `gpu_open()` fails and drawing stays in software; a
  Metal backend behind the same `gpu.h` would be the follow-up.
- **No GPU access on Linux** (containers, users outside the `render`
  group, remote sessions without a render node): software, with the
  reason printed.
- **Read-back stalls** on discrete GPUs: to be measured in milestone 4.
- **Guest-visible differences:** a game that reads back what it drew
  (`accessDrawBuffer` isn't supported, but it could read a memory
  context's pixmap) sees slightly different pixels in real-time runs.
  That is accepted, as such runs aren't reproducible anyway.
- **Late start (settled):** Mac OS can't create a 3D context before the
  Finder is up, and the GPU is ready long before, so a context never
  needs to switch backends once made.
