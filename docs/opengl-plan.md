# Plan: OpenGL for Mac OS 9 games

Mac OS 9 games draw with Apple's OpenGL 1.x through AGL, its Mac binding.
This plan gives syshm OpenGL libraries of its own, generated like the other
guest tools: they record the game's calls in guest memory and hand them to
the host in batches, where syshm does OpenGL's fixed-function work and
draws with the 3D backend the RAVE engine already uses, on the GPU in real
time and in software otherwise, so instruction-time runs stay reproducible.

3dfx Glide has a plan of its own, docs/glide-plan.md, on the same backend.
It should come first (see Order).

## Status

Milestones 0 (the groundwork, shared with Glide), 1 (AGL and a clear) and
2 (a triangle) are built; milestones 3 to 6 are to do. The user chose
OpenGL before Glide, and accepted the proposed decisions below for now
(replacing the libraries, recording in guest memory, fixed function in C
on the host, frames read back into guest memory, a Rage 128's identity).

Milestone 2:

- **Recording** (`src/opengl.c`): 317 of the 440 gl functions aren't host
  calls. Those that return nothing and take only values, or values and
  then one array whose size their name gives (the `v` forms of `glVertex`,
  `glColor`, `glNormal`, `glTexCoord`, `glMultiTexCoord*ARB`,
  `glSecondaryColor*EXT`, `glRasterPos`, `glIndex`, `glEdgeFlag`,
  `glEvalCoord`; `glLoadMatrix`, `glMultMatrix` and their transposes),
  have code of the library's own that appends a record to a 64 KB buffer
  in the process's instance of the library's data (zero-filled, its
  bounds at `OPENGL_DATA_RECORD`): a word, the function's entry << 16 | the
  record's size, then the arguments, each integer a word as its register
  (or past r10 the caller's parameter area) had it, each float the 4 bytes
  `stfs` stores, each double its 8, an array's elements each as such an
  argument (bytes and halfwords made words). `glVertex3f` records in 13
  instructions, as the Transport section has it; the code is generated
  from the function table (`record_code()`), and the library builds twice,
  once for its data's layout and once with the code. A full buffer, or the
  first call (the bounds start at 0 if the initialization routine hasn't
  set them), takes the entry's host call instead, and the host makes the
  same record from the registers and does it by the same code. Every host
  call of the library's (a swap, `glGetError`, `aglSetCurrentContext`)
  first does what's recorded, in order, to the current context, so errors
  and state come out as if every call had been one. Calls through a
  context's dispatch table are recorded too if it's the current context
  (else they're host calls, to that context). Not recorded: `glFlush` and
  `glFinish`, and what reads arrays then and there (`glArrayElement`,
  `glDrawArrays`, `glDrawElements`, `glDrawRangeElements`,
  `glLockArraysEXT`): unlike the Transport section's plan, `glArrayElement`
  is a host call, as a record of its index would read the arrays later. In
  the test application's triangle scene, a frame's 49 calls make one host
  call, the swap.
  Note: PowerPC's `stfs` doesn't round, it truncates; callers' floats are
  floats already, so that's exact, as the host's reading is (it reads them
  as `stfs` would).
- **The front end** (`src/glfront.c`, `src/glfront.h`), built without
  fused multiply-adds and with sines and cosines of its own (series in
  doubles after exact steps of 90 degrees), so its numbers are the same
  on every host: the matrix stacks (modelview 32 deep, projection and each
  texture unit's 8: the Rage 128's depths aren't known), in floats with
  Mesa's formulas (`glFrustum`'s and `glOrtho`'s arguments made floats
  first); vertices transformed by the projection times the modelview;
  all ten primitive modes assembled into points, lines and triangles, with
  OpenGL's provoking vertices for flat shading (the last; a polygon's
  first; a line loop's closing segment its first); clipping in clip
  coordinates against the near and far planes, and against the others
  only past a guard band of 4 view volumes (the raster state's clip is the
  viewport, cut by the scissor box); culling by each triangle's area in
  window coordinates; window coordinates through the viewport and depth
  range; current colors clamped per vertex; the raster state (depth
  function and mask, none without depth bits; blend factors, equation and
  color; the alpha test, where `GL_NEVER` draws nothing; color masks, and
  alpha kept at 1 without alpha bits) made again only when the state
  changes. 16-bit drawables are presented as the 5 top bits of 8, not
  dithered yet.
- **Where OpenGL leaves room, the Rage 128's way** (as far as known; in
  `glfront.h`): colors interpolated linearly in screen space (the chip's
  setup corrected texture coordinates for perspective, not colors);
  primitives partly off the screen drawn from their own vertices within the
  guard band, as hardware with one drew them; quads split as (v0, v1, v3)
  and (v1, v2, v3), as Mesa's software rasterizer of the time and (from
  memory) the r128 DRI driver emitted them. Today's Mesa (llvmpipe) splits
  them as (v0, v1, v2) and (v0, v2, v3): different only where a quad's
  colors aren't a linear function of its position.
- **OpenGL so far:** matrices (`glMatrixMode`, `glLoadIdentity`,
  `glLoadMatrix`, `glMultMatrix` and the transposes, `glTranslate`,
  `glScale`, `glRotate`, `glFrustum`, `glOrtho`, `glPushMatrix`,
  `glPopMatrix`, with their errors), `glActiveTextureARB` (for the texture
  matrices so far), `glBegin`/`glEnd` with every form of `glVertex`,
  `glColor`, `glNormal`, `glTexCoord`, `glMultiTexCoord*ARB`,
  `glSecondaryColor*EXT` and `glEdgeFlag` (found by name: the integer ones
  normalized for colors and normals as OpenGL 1.2's table 2.6 says),
  `glDepthFunc`, `glDepthMask`, `glDepthRange`, `glShadeModel`,
  `glCullFace`, `glFrontFace`, `glBlendFunc`, `glBlendEquation(EXT)`,
  `glBlendColor(EXT)`, `glAlphaFunc`, `glColorMask`, `glPointSize`,
  `glLineWidth`, `glHint` (taken), `glEnable`/`glDisable`/`glIsEnabled`
  for all of OpenGL 1.2's capabilities and its imaging subset's (kept;
  per texture unit where they are), `GL_INVALID_OPERATION` between
  `glBegin` and `glEnd` for what may not come there, and `glGet*` for all
  of that (`glGetIntegerv` scaling colors and such as OpenGL says). Not
  yet: textures, lighting, fog, `glRasterPos`, evaluators, polygon modes,
  offset and stipple, line stipple; lines are `raster.c`'s (a pixel a step,
  not OpenGL's diamond-exit rule).
- **`gpu.c`**: its varyings are `smooth` rather than `noperspective`
  (with w 1 everywhere, the same interpolation): Mesa's llvmpipe
  interpolated `noperspective` ones wrongly where it clipped a triangle at
  the target's edges, which OpenGL's triangles reach (found by the scene
  comparisons; `test_past_edges` in `tests/test_gpu.c`). RAVE's software
  pixels are untouched.
- **Checked against Mesa** (`tests/test_opengl.c`, on Linux): six scenes
  drawn through the libraries (by their host calls and records) in
  software and on the GPU, and by Mesa 25.2's compatibility profile
  (llvmpipe, a framebuffer object of EGL's surfaceless platform). Pixels
  differing by more than 2 out of 6,912:

  | scene | GPU | Mesa | what differs |
  |---|---|---|---|
  | gradient (orthographic, smooth, a quad) | 0 | 0 | |
  | depth (functions, mask, ties) | 1 | 1 | a pixel center on an edge |
  | flat (perspective, near-clipped floor) | 11 | 11 | centers on an edge of slope 1/3 |
  | smooth (the same, smooth) | 11 | 2,739 (by up to 141) | Mesa's colors corrected for perspective |
  | primitives (every mode, culling, viewport, scissor, points) | 0 | 0 | |
  | blend (blending, masks, alpha test) | 0 | 2 | alpha at the test's reference |

  The GPU and Mesa agree with each other on those edge pixels (both snap
  vertices to fixed point; `raster.c` doesn't): no pixel is drawn by one
  and not the other anywhere else.
- **The test application:** scene `triangle` (a 4:3 frustum, a floor
  from behind the near plane into the distance, a red-green-blue triangle
  and a yellow quad tilted through it, turned 25 degrees, a flat-shaded fan
  given partly by `glVertex3fv`), its floats passed rounded to single
  precision as a compiled caller passes them. On installed 8.5 in
  instruction time: the same screenshot on macOS and Linux (and `clear`'s
  the same as milestone 1's); on the GPU (Metal), 11 edge pixels differ;
  at 16 bits and on 8.6 it draws the same scene.
- **Tests:** `tests/test_glfront.c` (sines against libm; the matrices
  against references in doubles, stacks and errors; each mode's
  primitives and their order; flat shading's colors for every mode;
  culling for each face and mode; clipping by the near and far planes and
  the guard band, and the viewport and scissor as the clip; pixels drawn
  by `raster.c`: coverage, depth, masks, alpha test, blending);
  `tests/test_opengl.c` (the library's recording code run by the
  interpreter and the JIT: each record's bytes, a ninth argument from the
  parameter area, through the dispatch table, a full buffer, a context
  not current, a triangle drawn and swapped; the scenes above).
- Not yet: pacing swaps to the display's refresh as the 3D engine's frames
  are (with milestone 3, for Unreal Tournament); `GL_CLAMP` against Mesa
  (with textures); transform still runs at the drain on the emulation
  thread (the Host side section's backend thread is for milestone 6).

Milestone 1:

- **The libraries** (`src/opengl.c`, `src/opengl_table.h`): OpenGLLibrary
  (440 gl and 33 agl functions, and `glc_cc`), OpenGLMemory (25) and
  OpenGLUtility (51), with Apple's fragment names, exports and versions
  (current 1, oldest compatible 0), each export a host-call stub so far
  (recording comes with milestone 2). `tools/gltable.py` made the table
  from Mac OS 9.2.2's libraries (the exports, and the dispatch table's
  offset each gl wrapper calls through, read from its code) and the macOS
  SDK's headers (signatures; AGL's and OpenGLMemory's, which macOS lacks,
  are in the script). They're on the tools volume as "syshm OpenGL
  Library", "... Memory" and "... Utility" (type `shlb`, creator `syGL`);
  the installer doesn't offer them yet. Each process gets its own
  instance of OpenGLLibrary's data, which has its contexts, pixel formats
  and renderer infos (zero-filled), and the strings it hands out; its
  initialization and termination routines start and end the instance on
  the host. Unimplemented functions do nothing, and say so once.
- **The dispatch table** (verified): Mac OS 9.2.2's wrappers each call
  through the slot at a fixed offset of the context (`glc_cc` points at it,
  its first word is passed first): 441 slots from +0x4 to +0x6E4, in the
  order of Mac OS X's `GLIFunctionDispatch` (`gliDispatch.h`) but for
  +0x544, `blend_func_separate` there, which Mac OS 9 doesn't export, and
  +0x650 and +0x654, which are `glBlendColor` and `glBlendEquation` rather
  than Mac OS X's vertex array range functions. syshm's contexts are laid
  out so (their handle is their own address), and each slot calls the gl
  function with the handle first.
- **AGL:** one renderer (ID 0x00021000, accelerated, 32 MB of video and
  texture memory, window, full-screen and offscreen drawables, single and
  double buffers, depth 16/24/32, stencil 8 (not drawn yet), RGB 555 and
  888); `aglChoosePixelFormat` takes RGBA formats on 16- or 32-bit devices
  (the main device if none are given) and refuses accumulation, auxiliary,
  stereo and multisample buffers, other renderer IDs, and 8-bit screens;
  formats and renderer infos describe themselves; contexts, the current
  one (`glc_cc`), `aglCopyContext` (all state), drawables (a color port:
  its `portRect`, or with `AGL_BUFFER_RECT` that rectangle; an offscreen
  buffer), `aglUpdateContext`, `aglSwapBuffers`, swap rectangles, the swap
  interval kept; `aglGetVersion` 1.1 and Apple's error strings, as Mac OS
  9.2.2's. Not yet: `aglSetFullScreen`, `aglUseFont`.
- **Drawing and presenting** (`src/surface.c`): a context's buffers are a
  3D backend surface, drawn by the GPU or `raster.c` as RAVE's are; a swap
  (or `glFlush` and `glFinish` single-buffered) draws what's recorded and
  copies it into the port's pixmap (16 or 32 bits), clipped to the port's
  `visRgn` and `clipRgn` themselves (QuickDraw's region format is walked
  row by row), or into the offscreen buffer (alpha kept).
- **OpenGL so far:** `glClear` (color and depth, within the scissor box,
  with the color and depth masks), `glClearColor`, `glClearDepth`,
  `glViewport` and `glScissor` (set to the drawable's size when first
  attached), `glEnable`/`glDisable`/`glIsEnabled` (the scissor test),
  `glColorMask`, `glDepthMask`, `glGetString` (vendor "ATI Technologies
  Inc.", renderer "Rage 128 OpenGL Engine", version "1.2 syshm", no
  extensions yet), `glGetError`, `glGet*` for those and a few limits,
  `glFlush`, `glFinish`. OpenGLMemory: `glmMalloc` and the rest through
  `NewPtr`, `NewPtrClear`, `GetPtrSize` and `DisposePtr`; the fills and
  copies on the host; modes and settings accepted. OpenGLUtility: nothing
  yet.
- **The test application** (`src/gltest.c`, "syshm GL Test", on the tools
  volume with `SYSHM_TOOLS_GL_TEST=1`): logic in C on the host making
  callouts through its imports (InterfaceLib's, OpenGLLibrary's), so the
  Code Fragment Manager links it to the libraries as a game. It sets the
  screen's depth, opens a 320x240 window, makes a double-buffered context
  in it, prints the strings, and draws its scene at once and at each
  update event. Scene `clear`: the window cleared to (0.25, 0.5, 0.75),
  and a 100x60 orange rectangle 20 pixels in from the bottom left
  (scissored). Checked on installed 8.5 and 8.6 (libraries in Extensions,
  the application in Startup Items, put there with hfsutils on APFS
  clones), in instruction time: the window's pixels exact (0x4080BF and
  0xFF8000 at 32 bits; at 16 bits their 5-bit forms), the screenshot the
  same in two runs, with `SYSHM_GPU=1`, and on Linux (Mesa) as on macOS.
- **Tests:** `tests/test_opengl.c` (the fragments; AGL's formats, renderer
  info, errors and strings; a context's layout; a clear and a scissored
  one swapped into a window's port through a `clipRgn` with a hole, at 32
  and 16 bits, with a swap rectangle, and into an offscreen buffer; calls
  through the dispatch table; OpenGLMemory's callouts), in software and on
  the GPU; the builder's TVector relocations in runs of at most 512
  (`tests/test_tools.c`).
- Not checked yet: Mac OS 9.x, where Apple's same-named libraries are in
  Extensions too (the user's 9.2.2 image was in use).

Milestone 0:

- **The 3D backend** (`src/raster.h`, `src/raster.c`, `src/gpu.c`):
  vertices carry three colors (the primary one; OpenGL's secondary color,
  or RAVE's kd and ks), a fog coordinate (divided by w), and two texture
  units' s/w, t/w and q/w. A state has two texture units (OpenGL's six
  filters, clamping or repeating, coordinates divided per pixel by their
  own q/w or by 1/w) combined in up to four stages as `GL_COMBINE` does
  (replace, modulate, add, add signed, interpolate, subtract; arguments
  from the stage before, the colors, either unit's texture or a constant,
  their alpha in every channel, or one minus that; scaled and clamped or
  not), blend equations (add, subtract, reverse subtract, min, max) with
  the constant-color factors, no blending, color masks (clears included),
  fog by the fog coordinate, and fragments without alpha drawn as OpenGL
  draws them or skipped as RAVE's engines do. RAVE's texture operations
  are stages too (`raster_rave_stages()`), and its pixels are unchanged:
  20 random streams of RAVE calls (6,000 frames, 3 million triangles,
  every kind of call and state) give the same bytes as before, on macOS
  and on Linux. On the GPU each way of drawing gets a program of its own,
  made the first time it's needed (a few milliseconds); 4 of those 6,000
  frames differ from before, each in one pixel's channel, by one step. In software, drawing with
  one or two stages takes as long as before or up to 5% longer, with three
  (RAVE's modulate and highlight, ATI's multitexturing) 10-20% longer.
- **Reproducible software pixels:** `raster.c` is built without fused
  multiply-adds. Clang used 345 of them on Apple silicon, so RAVE's pixels
  there differed from Linux's and Intel Macs'; they're the same everywhere
  now (Linux's didn't change).
- **The fragment builder** (`src/ndrv.c`): any number of entries and
  exports (looked up through a hash table as PEF linkers lay it out:
  `PEFHashTableIndex`, checked against all 474 of Apple's OpenGLLibrary's
  exports), data exports, code of a fragment's own for chosen entries
  (which can branch to any entry's host-call stub), zero-filled data after
  the initialized, a termination routine, and the container's versions.
- **Host calls** see f1 to f13 as well as r3 to r10, and `ndrv_args`
  walks a call's arguments where the calling convention puts them,
  floats and doubles included, and words past r10 in the caller's
  parameter area.

Before that, research and design only. Facts checked on the media are
marked **verified**, with where they come from; the rest is from memory or
from documents found on the web, and is marked so. The media were read on
APFS clones with hfsutils; `tools/pef.py` (with a few lines more for weak
flags and versions) listed imports and exports, capstone disassembled,
`unar` opened the UT CD's Compact Pro archive, and the `arc-mac` crate's
tome reader unpacked the 9.1 installer's files. Mac OS 9.2.2's OpenGL
(on the user's installed 9.2.2, which is HFS+, read through macOS) is
1.2.2: OpenGLLibrary is 233,966 bytes, with the same 474 exports, current
version 1, oldest compatible definition 0, initialization and termination
routines, and 7,116 bytes of zero-filled data. Unreal Tournament's OpenGL
build imports every library with versions 0 and 0, so any version will do.

## Decisions (accepted for now)

- **Replace the libraries:** syshm provides OpenGLLibrary (gl and agl),
  OpenGLMemory and OpenGLUtility (glu) under Apple's fragment names, with
  the same exports, and they forward to the host. syshm doesn't load
  Apple's OpenGL engine or its renderers. This works the same on 8.5 and
  8.6, which have no OpenGL, as on 9.x.
- **Record in the guest:** most calls (vertices, colors, state) are stored
  in a buffer in guest memory by a few generated PowerPC instructions, with
  no kernel exit. A host call drains the buffer when it fills, and for any
  call that returns something or reads client memory.
- **Fixed function in syshm, in C:** the host tracks the GL state and does
  transform, lighting, texture coordinate generation and clipping itself,
  then hands screen-space primitives with their fragment state to the
  existing backends, `raster.c` (software, the reference) and `gpu.c`
  (OpenGL 3.3 core), both extended for OpenGL's per-fragment operations.
  No compatibility-profile context on the host.
- **Frames go back into guest memory**, into the drawable (window, memory
  or the whole screen), as the RAVE engine does. Presenting full-screen
  contexts straight in syshm's window comes later, as an optimization.
- **Identity:** an ATI renderer, "Rage 128 OpenGL Engine" with ATI's
  vendor string, as the RAVE engine reports one (see Identity).
- **First target:** Unreal Tournament's OpenGL build, on the UT CD, which
  needs 45 functions.
- **Order:** OpenGL first (the user's choice; this plan proposed Glide
  first, see Order). Milestone 0's groundwork is Glide's too.
- **Installation:** on 9.x syshm's libraries replace Apple's (moved to
  "Extensions (Disabled)" when they're installed); on 8.5 and 8.6 they're
  added.

## What Mac OS 9 ships

### Where it is (verified)

- **Mac OS 9.1 CD (`91.iso`):** the bootable System Folder (what syshm
  starts from the CD) has no OpenGL. The installer's archive, "Installation
  Tome" (`Software Installers:System Software:Mac OS 9.1`, 46,787,286
  bytes, type `idcp` creator `kakc`), holds six OpenGL files, all modified
  5 October 2000, each with `vers` 1 "1.2, Copyright Apple Computer, Inc.
  1999-2000" and `vers` 2 "Mac OS 9.1": so 9.1 installs Apple's OpenGL
  1.2. The tome compresses files with InstaCompOne; the `arc-mac` 0.2.0
  crate's tome reader (Rust, from rusty-backup) decoded them correctly (the
  code disassembles cleanly); dexvert's Python decoder corrupted them after
  the first 48 KB or so.
- **The 8.6 install (`hd86.img`):** no OpenGL. Its Extensions have
  QuickDraw 3D and RAVE, ATI's RAVE engine ("ATI 3D Accelerator", 26 March
  1999), the Game Sprockets and syshm's 3D engine.
- **The Unreal Tournament CD (`ut.iso`):** `Extras:OpenGL UT.sea`, a
  Compact Pro archive with an OpenGL build of the game (13 December 1999)
  and instructions: copy it and `OpenGLDrv.ini` into the game's folder and
  set `GameRenderDevice=OpenGLDrv.OpenGLRenderDevice`; "OpenGL can require
  from 40 to 80 MB of more total memory than RAVE". The installed
  application (4.32, on `hd86.img`) has only the Glide and RAVE renderers.

From memory, not verified here: 8.5 and 8.6 had no OpenGL, which Apple
offered as separate downloads (1.0 in 1999, later 1.1.2 and 1.2.1); 9.0
installed 1.1.2; 9.2.x installed 1.2.2 (MacGLide's notes and xlr8yourmac's
3dfx pages both name "OpenGL 1.2.2" with 9.2.x).

### The files (verified, Mac OS 9.1)

All are type `shlb` with one PowerPC fragment in the data fork, unless
said otherwise.

| File | Creator | Fragment | Data fork | Exports | Imports |
|---|---|---|---|---|---|
| OpenGLLibrary | `opgl` | OpenGLLibrary | 218,148 | 474: 440 `gl`, 33 `agl`, `glc_cc` (data) | OpenGLMemory 4, InterfaceLib 40, OpenGLProfiler 35 (weak) |
| OpenGLEngine | `gli1` | OpenGLEngine | 711,795 | 17 `gli` | OpenGLMemory 11, MathLib 6, InterfaceLib 7 |
| OpenGLMemory | `glm1` | OpenGLMemory | 10,370 | 25 `glm` | DriverServicesLib 2 (weak), InterfaceLib 6, MPLibrary 2 (weak) |
| OpenGLUtility | `glu1` | OpenGLUtility | 214,786 | 51 `glu` | MathLib 6, OpenGLLibrary 24, InterfaceLib 3, OpenGLMemory 3 |
| OpenGLRenderer | `gld1` | six (below) | 2,833,014 | 22 `gld` | OpenGLMemory 3, MathLib 2, GLSystem 15 |
| OpenGLRendererATI | `gld1` | OpenGLRendererATI | 278,388 | 22 `gld` | InterfaceLib 9, MathLib 2, QuickDraw™ 3D Accelerator 12, OpenGLMemory 5 |

OpenGLRenderer's `cfrg` has six members: OpenGLRenderer (the `gld`
interface), GLSystem (15 `gls` functions: drawables, buffers, color maps,
swaps) and four rasterizers by pixel format, GLRendererARGB8888D32,
GLRendererIndex8D32, GLRendererRGB555A8D16 and GLRendererRGB555A8D32.

### How the pieces fit (verified by disassembly)

- **OpenGLLibrary** is the API. 432 of its 440 `gl` functions are short
  wrappers: they load `glc_cc` (a data export, the current context), pass
  the context's first word as the first argument and call through a table
  in the context at a fixed offset (`glAccum` at +0x4, `glBegin` +0x14,
  `glVertex3f` +0x504, ... `glResetMinmax` +0x6D8). The other eight
  (`glTexImage2D`, `glTexSubImage2D`, `glTexImage3D`, `glTexSubImage3D`,
  `glCopyTexSubImage3D` and three compressed-texture ones) take arguments
  on the stack and do the same in longer code. The table's order is the
  GL 1.1 functions alphabetically, then the extensions, then GL 1.2's,
  which (from memory) is the order of `GLIFunctionDispatch` in Mac OS X's
  public `gliDispatch.h`: Mac OS X's OpenGL descends from this one.
- An `AGLContext` points at that structure: `aglSetCurrentContext` checks
  the context and stores it in `glc_cc` (or a "no context" table for
  NULL). So a program could call through the table directly (Mac OS X's
  `CGLMacro.h` does that; whether a Mac OS 9 `aglMacro.h` existed and was
  used is an open question).
- OpenGLLibrary finds the engine at run time (it imports `GetSharedLibrary`
  and `FindSymbol`; its data holds the 17 `gli` names). It also draws text
  for `aglUseFont` with QuickDraw (`NewGWorld`, `DrawChar` and friends).
- **OpenGLEngine** (the `gli` interface: pixel formats, renderer info,
  contexts, dispatch tables, drawables, swaps) does the state machine and,
  going by its size, its math imports and the renderers' interface,
  transform and lighting. It finds renderers by walking a folder and its
  subfolders, one level deep (`FindFolder`, `PBGetCatInfoSync`), for files
  whose creator is `gld1` (the comparison is at its code + 0x390), loads
  each with `GetDiskFragment` and looks up the 22 `gld` names. Its
  extension list (strings in its code): `GL_APPLE_specular_vector`,
  `GL_APPLE_transform_hint`, `GL_APPLE_packed_pixel`,
  `GL_ARB_transpose_matrix`, `GL_EXT_clip_volume_hint`,
  `GL_EXT_rescale_normal`, `GL_NV_texgen_reflection`,
  `GL_ARB_multitexture`, `GL_ARB_texture_env_add`, `GL_EXT_blend_color`,
  `GL_EXT_blend_minmax`, `GL_EXT_blend_subtract`,
  `GL_EXT_compiled_vertex_array`, `GL_EXT_texture_env_add`,
  `GL_EXT_texture_env_combine`, `GL_EXT_texture_lod_bias`, `GL_EXT_abgr`,
  `GL_EXT_bgra`, `GL_SGIS_texture_edge_clamp`.
- **Renderers** export the 22-function `gld` interface (`gldGetVersion`,
  `gldGetRendererInfo`, `gldChoosePixelFormat`, `gldCreateContext`,
  `gldAttachDrawable`, `gldGetRenderDispatch`, `gldAllocVertexBuffer`,
  `gldCompleteVertexBuffer`, `gldBeginPrimitiveBuffer`, the texture calls,
  `gldGetString` and so on): the engine hands them transformed vertices.
  Renderer IDs: `gldGetRendererInfo` stores 0x200 (Apple's) or 0x1000
  (ATI's) and the engine ORs in 0x20000, giving 0x00020200 (Apple's
  generic software renderer) and 0x00021000 (ATI).
- **Apple's software renderer** reports vendor "Apple", renderer "Generic"
  and the extensions `GL_ARB_texture_env_add GL_EXT_blend_color
  GL_EXT_blend_minmax GL_EXT_blend_subtract GL_EXT_texture_env_add
  GL_EXT_texture_lod_bias GL_EXT_abgr GL_EXT_bgra
  GL_SGIS_texture_edge_clamp`.
- **ATI's renderer draws through RAVE**: it imports 12 functions from
  "QuickDraw™ 3D Accelerator" and takes, of a device's RAVE engines, one
  with `kQAOptional_BoundToDevice` (optional-features bit 12) and vendor 1
  (ATI) that passes `QAEngineCheckDevice`. It asks the engine's ID,
  revision, name, texture memory and texture types, and three private ATI
  gestalt selectors (1001, with an argument; 1002, which returns a table of
  functions it calls; 1005, flags). It sets about 50 private ATI
  draw-context tags (1000-1094, mostly through `setInt`, a few through
  `setFloat` and `setPtr`; 1002-1009 carry fog), reads tags 1011 and 1021
  (the hardware generation and ATI's extension table, which syshm's engine
  answers for UT), and uses RAVE's `accessDrawBuffer` and `accessZBuffer`,
  which syshm's engine doesn't support. Its strings: vendor "ATI
  Technologies Inc."; renderers "Rage II OpenGL Engine", "Rage Pro OpenGL
  Engine", "Rage Mobility OpenGL Engine", "Rage 128 OpenGL Engine", "ATI
  Radeon OpenGL Engine", "ATI 3DRage OpenGL Engine"; version
  `1.1.ATI-%d.%d`; and three extension lists by chip:
  - `GL_ARB_texture_env_add GL_EXT_compiled_vertex_array
    GL_EXT_texture_env_add GL_EXT_abgr GL_EXT_bgra`
  - the same plus `GL_ARB_multitexture GL_EXT_texture_lod_bias` (from the
    order of the strings, presumably the Rage 128)
  - that plus `GL_EXT_texture_env_combine GL_SGIS_texture_edge_clamp`
    (presumably the Radeon)

  9.1's installer also carries ATI's RAVE engines for these chips ("ATI 3D
  Accelerator", "ATI Rage 128 3D Accelerator", "ATI Radeon 3D
  Accelerator").

### The API surface (verified)

- **OpenGLLibrary, 440 `gl` functions:** GL 1.1; GL 1.2's additions
  (`glDrawRangeElements`, the 3D texture calls, and the imaging subset's
  color tables, convolution, histogram and minmax, `glBlendColor`,
  `glBlendEquation`); and the entry points of `ARB_multitexture` (both
  `glActiveTextureARB` and `glClientActiveTextureARB`, the 32
  `glMultiTexCoord*ARB`), `ARB_transpose_matrix`, `ARB_texture_compression`
  (whose extension 9.1's engine doesn't list), `EXT_blend_color`,
  `EXT_blend_minmax`, `EXT_compiled_vertex_array` and `EXT_secondary_color`
  (17 functions, also unlisted).
- **33 `agl` functions**, in OpenGLLibrary (there is no separate AGL
  library): `aglChoosePixelFormat`, `aglConfigure`, `aglCopyContext`,
  `aglCreateContext`, `aglDescribePixelFormat`, `aglDescribeRenderer`,
  `aglDestroyContext`, `aglDestroyPixelFormat`, `aglDestroyRendererInfo`,
  `aglDevicesOfPixelFormat`, `aglDisable`, `aglEnable`, `aglErrorString`,
  `aglGetCurrentContext`, `aglGetDrawable`, `aglGetError`,
  `aglGetInteger`, `aglGetVersion`, `aglGetVirtualScreen`, `aglIsEnabled`,
  `aglNextPixelFormat`, `aglNextRendererInfo`, `aglQueryRendererInfo`,
  `aglResetLibrary`, `aglSetCurrentContext`, `aglSetDrawable`,
  `aglSetFullScreen`, `aglSetInteger`, `aglSetOffScreen`,
  `aglSetVirtualScreen`, `aglSwapBuffers`, `aglUpdateContext`,
  `aglUseFont`. Its error strings run from "invalid pixel format
  attribute" to "memory allocation failure".
- **OpenGLUtility:** 51 `glu` functions. **OpenGLMemory:** 25 `glm`
  functions (`glmMalloc`, `glmFree`, `glmSetMode` and so on), which
  programs may call too.

### How programs link (verified for UT)

Unreal Tournament's OpenGL build imports, all **weak** (library option
0x40, so it starts without them and checks), 44 functions from
OpenGLLibrary and 1 from OpenGLMemory:

- `gl` (37): `glActiveTextureARB`, `glAlphaFunc`, `glBegin`,
  `glBindTexture`, `glBlendFunc`, `glClear`, `glClearColor`,
  `glClearDepth`, `glColor3fv`, `glColor4f`, `glColor4fv`, `glColorMask`,
  `glDeleteTextures`, `glDepthFunc`, `glDepthMask`, `glDepthRange`,
  `glDisable`, `glEnable`, `glEnd`, `glFlush`, `glFrustum`,
  `glGenTextures`, `glGetString`, `glLoadIdentity`, `glMatrixMode`,
  `glMultMatrixf`, `glMultiTexCoord2fARB`, `glReadPixels`, `glShadeModel`,
  `glTexCoord2f`, `glTexEnvf`, `glTexImage2D`, `glTexParameteri`,
  `glTexSubImage2D`, `glVertex3f`, `glVertex3fv`, `glViewport`.
- `agl` (7): `aglChoosePixelFormat`, `aglCreateContext`,
  `aglDestroyContext`, `aglDestroyPixelFormat`, `aglSetCurrentContext`,
  `aglSetDrawable`, `aglSwapBuffers` (onto DrawSprocket's front buffer:
  it imports `DSpContext_GetFrontBuffer` and friends, and not
  `aglSetFullScreen`).
- `glmSetMode` from OpenGLMemory.

It logs `GL_VENDOR`, `GL_RENDERER`, `GL_VERSION` and `GL_EXTENSIONS` and
looks for `GL_ARB_multitexture`, `GL_EXT_bgra`, `GL_S3_s3tc`,
`GL_EXT_compiled_vertex_array`, `GL_EXT_paletted_texture`,
`GL_EXT_clip_volume_hint`, `GL_WIN_swap_hint` and `WGL_EXT_swap_control`
(only multitexture's functions are imported). `OpenGLDrv.ini` picks
settings by "vendor/renderer" sections (`[ATI/RAGE 128]`, `[3Dfx
Interactive Inc./3Dfx]`; the Mac build's copy has only `[Default]`). It
never calls `glGetError` or `glGet*`.

### Choosing a renderer (from memory, with the verified IDs)

A program passes attributes (`AGL_RGBA`, `AGL_DOUBLEBUFFER`,
`AGL_DEPTH_SIZE`, `AGL_ACCELERATED`, `AGL_NO_RECOVERY`,
`AGL_RENDERER_ID`, `AGL_FULLSCREEN`, `AGL_OFFSCREEN`,
`AGL_ALL_RENDERERS`, policies, ...) and optionally a list of GDevices to
`aglChoosePixelFormat`; the engine asks each renderer for matching
formats. `AGL_ACCELERATED` with `AGL_NO_RECOVERY` rules out Apple's
software renderer, and is how games insist on hardware.
`aglQueryRendererInfo` and `aglDescribeRenderer` (`AGL_RENDERER_ID`,
`AGL_ACCELERATED`, `AGL_VIDEO_MEMORY`, `AGL_TEXTURE_MEMORY`, the buffer,
color, depth and stencil modes) let a program look before choosing. The
IDs on 9.1 are 0x00020200 (generic) and 0x00021000 (ATI), verified above.

### Is the renderer interface documented?

Not publicly, as far as is known. `gli` and `gld` are Apple's private
interfaces, though other vendors had renderers for it (from xlr8yourmac's
3dfx pages and MacGLide's notes: 3dfx's native OpenGL for Voodoo cards,
over Glide 3; NVIDIA's for the GeForce 2 MX on 9.2.2). Mac OS X kept both
names (`gliDispatch.h` is public there, the `gld` driver interface isn't).
The 9.1 CD has two working renderers and the engine, so the interface could
be learned from them, at a cost (option (b) below).

## Options

**(a) Replace the libraries (recommended).** syshm generates OpenGLLibrary,
OpenGLMemory and OpenGLUtility with Apple's fragment names and exports;
their calls go to the host, which does everything.

- For: speed (no OpenGL engine running as emulated PowerPC code; vertices
  transformed by the host); works on 8.5 and 8.6 without Apple's OpenGL,
  so UT's OpenGL build would run on the user's 8.6 install as is; syshm
  controls the identity, extensions and limits; one deterministic software
  path; the same backend as RAVE and Glide.
- Against: syshm implements OpenGL 1.2's fixed-function semantics itself,
  the largest part of the work; Apple's own OpenGL is set aside on 9.x.

**(b) A renderer under Apple's engine.** A `shlb` of creator `gld1` that
Apple's engine loads like ATI's, exporting the 22 `gld` functions.

- For: Apple's engine keeps all the API and state semantics.
- Against: the interface is private and would have to be learned from the
  engine and two renderers (vertex buffer layouts, the render dispatch
  table, textures, drawables); transform and lighting stay in emulated
  PowerPC code; 9.x only (or wherever Apple's OpenGL is installed).

**(c) Apple's OpenGL over syshm's RAVE engine.** Apple's ATI renderer
already turns OpenGL into RAVE calls; syshm's engine would have to pass for
an ATI engine to it.

- For: no new library; syshm's RAVE path is deterministic already.
- Against: much private ATI interface to learn (gestalts 1001, 1002 and its
  function table, 1005; some 50 private tags; `accessDrawBuffer` and
  `accessZBuffer`), from ATI's RAVE engines on the 9.1 CD; OpenGL limited
  to what a Rage's RAVE engine can do (Apple's ATI renderer reports
  OpenGL 1.1); transform, lighting and RAVE overhead in emulated code; 9.x
  only.

**(d) Forward calls to the host's OpenGL** (a compatibility-profile
context), as the host side of (a). See Host side: it's the quickest way to
pixels (and what Glide wrappers such as OpenGLide and MacGLide do with
Glide), but it has no reproducible software path.

Recommendation: (a), with the host side in Host side below. (c) stays
possible later if running Apple's own OpenGL on 9.x matters for some
program; nothing in (a) gets in its way.

### Installation and coexistence

- The libraries join the guest tools volume as one folder ("syshm OpenGL":
  three files with syshm's own creator and file names, fragment names
  OpenGLLibrary, OpenGLMemory and OpenGLUtility, `cfrg` versions as
  Apple's: current 1, oldest compatible 0, so programs linked against
  Apple's, including Apple's OpenGLUtility, accept them). The install
  prompt (another branch) puts them in Extensions.
- On 9.x, Apple's same-named libraries are in Extensions too. Rather than
  rely on which of two same-named fragments the Code Fragment Manager
  picks (an open question to test), the install prompt moves Apple's
  OpenGLLibrary, OpenGLMemory and OpenGLUtility to "Extensions
  (Disabled)", saying so, and moves them back on uninstall. Apple's engine
  and renderers are then never loaded (only Apple's OpenGLLibrary loads
  the engine).
- As with the 3D engine, the library's data carries its version, so one
  installed by an older syshm is noticed and the user told to install it
  again.
- 8.5 and 8.6: the same files, nothing to disable.

## Design

### The libraries in the guest

They're generated PEF containers, like the RAVE engine (`ndrv_build()` in
`src/ndrv.c`, `src/tools.c`), from one table of the 474 + 25 + 51
functions with their C signatures. The builder needs to grow:

- More than 64 entries and exports, and data exports (`glc_cc`).
- Code per export instead of `li r11,k; b common` for all: the recording
  stubs below, and plain host-call stubs for the rest.
- Zero-filled data (a section whose total size exceeds its initialized
  size), for the recording buffer.

**The calling convention** is the Mac OS (AIX-style) PowerPC one, and the
table derives each argument's place from the signature: integers and
pointers take the next word (r3-r10, then the caller's parameter area at
24(r1) + 4 × word); floats and doubles take the next of f1-f13 and also
skip one or two words. So `glBitmap(w, h, xorig, yorig, xmove, ymove,
bitmap)` has `w` in r3, `h` in r4, the four floats in f1-f4 and `bitmap`
in r9, and `glTexImage2D`'s ninth argument is on the stack. Host calls need
more than f1, which is all `struct ndrv_call` carries today: the kernel
passes the handler f1-f13 too.

**Contexts:** `aglCreateContext` returns a pointer to a structure laid out
as Apple's: word 0 a handle, then TVector pointers at Apple's offsets
(+0x4 to +0x6E4: see Status, milestone 1), whose entries take that handle
first (generated variants of the same stubs). `glc_cc` is exported as Apple's
is. Programs that call through the table then work too, at little cost.

**Per-process data:** like any `shlb` with process-shared data, each
application gets its own instance of the data section (current context,
recording buffer); the host keys its state by the instance (its TOC).

**OpenGLMemory** is trivial (`glmMalloc` and friends over `NewPtr` through
callouts, `glmSetMode` accepted). **GLU** runs on the host: the matrix
helpers, `gluBuild2DMipmaps` and `gluScaleImage`, `gluProject` and
`gluUnProject`, quadrics (which emit GL commands into the same stream),
`gluErrorString` and `gluGetString`. The tessellator (its callbacks are
guest code, so callouts) and NURBS come later, if a program needs them.

### Transport

**Recording.** Calls that return nothing and read no client memory beyond
a few fixed-size values are recorded by their stubs in a buffer in the
library's data: an opcode word and the arguments, already in the form the
host reads. `glVertex3f(x, y, z)` is about a dozen instructions:

```
lwz   r11, rec_ptr(r2)          # where the next command goes
lwz   r12, rec_end(r2)
addi  r0, r11, 16
cmplw r0, r12
bgt-  full                      # no room: a host call drains the buffer, then retry
li    r12, OP_VERTEX3F
stw   r12, 0(r11)
stfs  f1, 4(r11)                # (exact: the arguments were floats to begin with)
stfs  f2, 8(r11)
stfs  f3, 12(r11)
stw   r0, rec_ptr(r2)
blr
```

That covers the immediate-mode calls (`glBegin`, `glEnd`, `glVertex*`,
`glColor*`, `glTexCoord*`, `glNormal*`, `glMultiTexCoord*ARB`,
`glSecondaryColor*EXT`, `glArrayElement`'s index), matrices (`glLoadMatrixf`
and `glMultMatrixf` copy their 16 values), and most state (`glEnable`,
`glBlendFunc`, `glBindTexture`, `glTexEnvf`, `glDepthMask`, `glFogf`, ...).
The `v` forms copy their few values. A buffer of 64 KB holds a few
thousand vertices.

**Host calls** (an exit each, after draining the buffer): anything that
returns a value (`glGet*`, `glIs*`, `glGetError`, `glGenTextures`,
`glGenLists`, `glRenderMode`, `agl*`), reads client memory of variable size
(`glTexImage*`, `glTexSubImage*`, `glDrawPixels`, `glBitmap`,
`glDrawArrays` and `glDrawElements` with client arrays, `glCallLists`) or
writes it (`glReadPixels`, `glGetTexImage`, feedback and selection
buffers), and the sync points (`glFinish`, `glFlush`, `aglSwapBuffers`).

**On the host, at each drain**, on the emulation thread: the commands are
decoded in order into the GL state (so `glGet*`, `glIsEnabled` and
`glGetError` are answered at once, from it), and those that draw become a
host batch (native byte order, client data copied) for the backend
thread. That's cheap, and keeps state and errors exactly in call order.
Transform, lighting, clipping and drawing happen on the backend thread
while the guest records the next batch, as RAVE batches overlap now.

**Client arrays:** `glVertexPointer` and friends only record pointers and
strides. A draw call copies the elements it uses (`glDrawArrays`' range;
for `glDrawElements` the index range, found by scanning the indices) from
guest memory at the call, converting from big-endian. Between
`glLockArraysEXT` and `glUnlockArraysEXT` the locked range is copied
once and transformed once for every draw that uses it (from memory,
Quake III draws each surface in several passes this way).

**Pixels:** `glPixelStore` unpacking (alignment, row length, skips, swap
bytes, LSB first) is done by the copy. Packed types (`GL_UNSIGNED_SHORT_5_6_5`,
`4_4_4_4`, `1_5_5_5_REV`, `8_8_8_8` and the rest) are big-endian in the
guest and are converted word by word; textures are stored as RGBA8 on the
host (both backends take that), keeping the internal format for queries.
`glReadPixels` waits for the backend, then converts into guest memory.

**Errors:** decoding sets GL's error flags as it goes, so `glGetError`
returns the first error since the last call, as on a real implementation.
Errors in recorded calls surface at the next drain, which is still in
order.

**Display lists:** owned by the host. Between `glNewList` and `glEndList`
decoded commands are stored in the list (and executed too with
`GL_COMPILE_AND_EXECUTE`); `glCallList` is recorded like any command and
replays the list on the host.

**Textures** are host objects: names, parameters, images per level; the
backend keeps its own copy (a GPU texture or `raster_texture`), uploaded
when first used after a change, as RAVE's are.

**Cost model.** A kernel exit costs about 50 ns (README: device register
accesses on an M5). A recording stub is a dozen instructions, a few
nanoseconds once the JIT has translated it (the JIT runs the 68k
emulator's code at about 2.5 billion PowerPC instructions a second). UT's
OpenGL renderer presumably draws each surface (going by its imports) as
`glBegin`, three calls per vertex (`glTexCoord2f`, `glMultiTexCoord2fARB`,
`glVertex3f`) and `glEnd`: at an estimated 50,000 calls a frame that's
2.5 ms of exits a frame, against about 0.25 ms recorded, plus the host's
decoding (tens of nanoseconds a command) either way. Exits alone would be
playable at 60 frames a second; recording leaves the time to the game,
and matters more for heavier immediate-mode programs. Quake III-style
vertex arrays cost one exit per draw call and a copy of its vertices.

### Host side

The fixed-function pipeline needs a home. Two ways:

**(i) Forward to a compatibility-profile context:** replay the calls on
macOS's legacy OpenGL 2.1 context, or Mesa's or NVIDIA's compatibility
profile on Linux, and let the driver do fixed function.

- For: the least code; the driver's semantics (and selection, feedback,
  accumulation, evaluators) for free; quickest to pixels.
- Against: no reproducible software path: Mesa's llvmpipe differs between
  versions and hosts, and macOS has none, so instruction-time runs would
  depend on the host. A second kind of context beside `gpu.c`'s core one,
  with its own loader and thread. The legacy profile is as deprecated on
  macOS as the core one, and if OpenGL goes, a Metal backend would then
  need fixed function written anyway. Two semantics (the driver's in real
  time, something else in software).

**(ii) Fixed function in syshm, on the core path (recommended).**

- **The front end, in C** (new, `src/glfront.c` or so), runs on the
  backend thread: vertex assembly (immediate mode, arrays, `GL_QUADS`,
  `GL_POLYGON`, strips and fans), the modelview and projection matrices,
  normals and `GL_NORMALIZE`/`GL_RESCALE_NORMAL`, lighting (8 lights,
  materials, color material, two-sided, local viewer, separate specular),
  texture coordinate generation (object and eye linear, sphere map, NV
  reflection) and the texture matrices, fog coordinates, user clip planes
  and frustum clipping, perspective division and the viewport, culling,
  polygon modes, polygon offset, flat shading, point and line widths and
  stipples, the raster position, `glBitmap` and `glDrawPixels`. Out come
  screen-space points, lines and triangles whose vertices carry x, y,
  depth, 1/w and, divided by w for perspective, colors, secondary color,
  fog and each texture unit's coordinates: the form RAVE's vertices
  already have, and Glide's.
- **The backends** (`raster.c` and `gpu.c`, shared with RAVE and Glide)
  gain the per-fragment pipeline: up to 4 texture units chained with
  OpenGL's environment modes (modulate, decal, blend, replace, add,
  combine), secondary color, fog, alpha test, stencil, depth with depth
  range and polygon offset, blending with blend color, min/max and
  subtract, logic ops, color masks, scissor and polygon stipple. In
  `gpu.c` these are uniforms of one shader, as RAVE's state is now; the
  vertex shader just passes screen-space vertices through, as it does for
  RAVE.

Why (ii):

- **Reproducible:** one front end feeds both backends, so instruction-time
  runs draw with `raster.c` from exactly the geometry the GPU would get.
  The front end is compiled with `#pragma STDC FP_CONTRACT OFF` (as
  `ppc_fp.c` is), so its floats are the same on x86_64 and AArch64.
- **macOS and Apple silicon:** it needs only the OpenGL 4.1 core context
  `gpu.c` already makes (Metal-backed on Apple silicon); if OpenGL leaves
  macOS, a Metal port of `gpu.c` carries RAVE, Glide and OpenGL together.
- **Linux:** the core profile is what Mesa and every driver support best,
  and CI already tests `gpu.c` on llvmpipe.
- **Reuse:** `gpu.c`'s thread, loader, acceptance checks, readback and
  fallback; `raster.c`'s banded threads.
- **Cost:** transform on the host's CPU is cheap at this era's vertex
  counts (tens of thousands a frame, tens of nanoseconds each). Moving it
  to a vertex shader is an optimization for later, with the C code staying
  the reference.

The price is the fixed-function semantics: this is most of the size
estimate. Two things contain it: milestones follow what the target games
call, and in CI on Linux, Mesa's compatibility profile serves as an
oracle: the tests draw the same command streams through syshm's front end
and through Mesa, and compare (`(i)` as a test, not as the product).

What's not planned: evaluators (`glMap*`, `glEvalCoord*`), the
accumulation buffer and the imaging subset's convolution, histogram and
minmax (they get `GL_INVALID_OPERATION`, and `GL_ARB_imaging` isn't
advertised); selection and feedback get a simple implementation (from
the front end's output) when a program needs them.

**Extensions to offer:** the engine's list above (all of it is within
reach), plus the secondary color and compressed texture extensions whose
functions are exported anyway, `GL_EXT_texture_compression_s3tc` and
`GL_EXT_paletted_texture` (decoded to RGBA8 on upload), and
`GL_EXT_texture_filter_anisotropic`. Limits: 2 or 4 texture units, 8
lights, 6 clip planes, 8 stencil bits, 16/24-bit depth, textures to 1024
or 2048 (see Identity).

### AGL and presentation

- **Pixel formats and renderer info:** one renderer, accelerated, ID
  0x00021000 (as ATI's on 9.1), window, full-screen and offscreen
  drawables, RGBA in 16 and 32 bits, depth 16/24/32, 8-bit stencil, no
  accumulation buffer, double or single buffering. `aglChoosePixelFormat`
  honors the attributes and policies, and fails for 8-bit indexed devices
  (Apple's software renderer drew to those; games switch to thousands or
  millions of colors). `aglDescribeRenderer` answers video and texture
  memory generously (see Identity).
- **Contexts:** creation, sharing (texture and display list namespaces
  shared between contexts created with a share context), `aglCopyContext`
  (attribute groups), destruction, `aglSetCurrentContext` (stores into
  `glc_cc`).
- **Drawables:** `aglSetDrawable(ctx, port)` takes a window's port. The
  host reads what it needs from the `CGrafPort` in guest memory (the port
  pixmap's bounds give the window's place on screen, `portRect` its size,
  `visRgn` and `clipRgn` what's visible), at `aglSetDrawable`,
  `aglUpdateContext` and each swap, so no callouts. `aglSetOffScreen`
  takes width, height, row bytes and a base address in guest memory.
  `AGL_BUFFER_RECT` and `AGL_SWAP_RECT` restrict to a rectangle.
- **Full screen:** `aglSetFullScreen(ctx, width, height, freq, device)`
  switches the display mode through the Display Manager (callouts, as the
  syshm Enhancements helper switches modes) and draws to the whole screen;
  destroying the context or setting another drawable switches back. Most
  games go full screen with DrawSprocket instead and attach to its front
  buffer's port, as UT does; that already works for RAVE.
- **Reading frames back** (the default): at a swap (or `glFlush` and
  `glFinish` when single-buffered) the frame is read back from the backend
  and written into the drawable in guest memory, converted to the screen's
  16 or 32 bits and clipped to the visible region, as RAVE's `present()`
  does (which today clips to the clip region's bounding box only; window
  drawables need the region itself, overlapped by other windows: a region
  walker shared with RAVE). Then everything downstream works as now:
  QuickDraw sees the pixels, screenshots include them, and instruction-time
  runs reproduce them. A frame is 5 MB at 1280x1024, which `gpu-plan.md`
  found nearly free to read back with Apple silicon's shared memory (and
  366 frames a second at 640x480, read back each time).
- **Presenting directly** (later, real time only): for a context covering
  the whole screen (full screen, or DrawSprocket's front buffer), syshm's
  window shows the backend's image itself, read back into host memory and
  handed to `display.c`, skipping the conversion into guest memory and the
  region clipping. Screenshots then take that image. Measured against
  reading back first; only kept if faster.
- **Vertical sync and pacing:** with `AGL_SWAP_INTERVAL` 1, a swap waits
  for the next display refresh in guest time (60 Hz): the swap's host call
  says how long, and the stub waits in a loop around a host call that naps
  like `PowerDispatch` (in real time until then or an interrupt; in
  instruction time it returns at once, so the loop counts instructions and
  stays reproducible). Without it, no waiting: the window shows at most 60
  frames a second anyway (`display_frame()`).
- **`aglUseFont`:** each glyph is drawn into a GWorld with QuickDraw
  through callouts (as Apple's library does) and becomes a display list of
  `glBitmap`.

### Identity

What games look at, from memory unless verified:

- `AGL_ACCELERATED` in pixel formats and renderer info; nearly every
  game asks for it (and `AGL_NO_RECOVERY`), so the answer is yes.
- The renderer ID (0x00021000 for ATI, verified), video and texture
  memory (`AGL_VIDEO_MEMORY`, `AGL_TEXTURE_MEMORY`): minimums such as 8 or
  16 MB. Proposed: 32 MB video, 32 MB texture.
- `GL_RENDERER` for quirks: Quake III treats "Rage Pro" specially (fewer
  features) and has paths for 3dfx and others; UT picks `OpenGLDrv.ini`
  sections by vendor and renderer (verified that it does; its Mac file has
  only `[Default]`).
- `GL_EXTENSIONS` for features (`GL_ARB_multitexture` above all).

Proposed strings: `GL_VENDOR` "ATI Technologies Inc." and `GL_RENDERER`
"Rage 128 OpenGL Engine" (Apple's own strings, verified), like the RAVE
engine, which tells UT it's a Rage 128; `GL_VERSION` "1.2" followed by
syshm's version (Apple's ATI renderer said `1.1.ATI-%d.%d`; programs parse
the leading number); `GL_MAX_TEXTURE_SIZE` 1024 and 2 texture units, as a
Rage 128 had (from memory); and the extension list above, which is more
than a Rage 128 had.
The alternative is "ATI Radeon OpenGL Engine" (2048 textures, more units),
for later games that expect a Radeon; or honest syshm strings, which
games of the time don't know. **The user decides.** Whatever is chosen,
`GL_EXTENSIONS` describes what syshm really does.

### Targets

Verified on the media: **Unreal Tournament** (OpenGL build on the UT CD,
45 functions, above).

From memory, to confirm per game (Mac OS 9 versions; several also ran on
Mac OS X):

- Quake III Arena (OpenGL only; vertex arrays, compiled arrays,
  multitexture; needs hardware). Return to Castle Wolfenstein and Star Trek
  Voyager: Elite Force on the same engine.
- Warcraft III (2002; whether its Mac OS 9 version needs more than 9.1 is
  to check).
- Deus Ex, Rune, Unreal (Unreal Engine 1, like UT: RAVE, Glide and, in
  some versions, OpenGL).
- Oni, Myth II (later versions) and Myth III, Otto Matic, Quake II,
  Heretic II, Soldier of Fortune, Heavy Metal: F.A.K.K.², Alice, Descent 3.
- Bugdom 2 and Nanosaur 2 were Mac OS X only, so they aren't targets.

Nearly all asked for an accelerated renderer; few would run on Apple's
software one at all.

**Test programs:** Apple's OpenGL SDK for Mac OS 9 (from memory: GLUT 3.7
for Mac OS and sample code; a Carbon version of Apple's "SetupGL" sample
is in MacGLide's repository, verified in its file tree) and GLUT demos.
syshm's own: a test application generated like syshm Enhancements, whose
logic is C on the host making callouts into the guest's OpenGLLibrary
(through the real CFM linkage), drawing known scenes and checked by
screenshots in instruction time. And UT's OpenGL build, a real game the
user has.

### Glide

docs/glide-plan.md. The same backend, presentation and pacing serve it;
Glide's vertices are already in screen space, so it skips the front end.

## Order: Glide or OpenGL first

Glide first:

- It's smaller: UT's Glide renderer needs 54 functions, Glide 2 has about
  125, with no transform, lighting, clipping or state queries. About a
  third of OpenGL's size.
- It builds most of what OpenGL needs, and tests it on a real game: the
  library builder with many exports, host calls with floats, the backend's
  multitexture combining, fog, alpha and chroma keying and depth modes,
  presentation, pacing and cleanup when a game quits.
- Its first target is installed already: Unreal Tournament 4.32 on the
  8.6 disk has its Glide renderer built in.
- Several Mac games have no accelerated path but Glide (Tomb Raider I-III,
  Carmageddon, Falcon 4.0; see the Glide plan), while most OpenGL games
  of the time also had RAVE or Glide.

OpenGL has more games, and the later ones; it comes second and reuses the
above, adding the front end, AGL and the transport.

## Milestones

Each leaves the tree working and `make test` passing on Linux and macOS.
Shared pieces may already exist from Glide.

0. **Groundwork** (shared; whichever comes first builds it): the PEF
   builder (exports, per-export code, zero-filled data); host calls with
   f1-f13 and stack arguments; the backend's richer vertices and fragment
   state, with RAVE moved onto them and its pixels unchanged byte for byte
   (as `gpu-plan.md` milestone 1 checked the seam).
1. **AGL and a clear.** The three libraries generated and installable;
   pixel formats, renderer info, contexts, `aglSetDrawable`,
   `aglSwapBuffers`; `glViewport`, `glClearColor`, `glClear`,
   `glGetString`, `glGetError`. Done when the generated test application
   clears a window to a color, and the screenshot is the same on every
   host in instruction time.
2. **A triangle.** Recording; matrices, `glFrustum`/`glOrtho`,
   `glBegin`/`glEnd`, colors, depth; the front end's transform and
   clipping. Done when known scenes match between `raster.c`, `gpu.c` and
   Mesa's compatibility profile (as oracle).
3. **Textures, then UT.** Texture objects, `glTexImage2D`,
   `glTexSubImage2D`, parameters, environments, blending, alpha test,
   multitexture, `glReadPixels`, `glmSetMode`. Done when UT's OpenGL build
   plays on the 8.6 disk (a clone), on the GPU and in software, and its
   screenshots match the RAVE renderer's in content.
4. **Arrays, fog, lighting, the rest of the common state.** Vertex
   arrays, `glDrawElements`/`glDrawRangeElements`, compiled arrays, fog,
   lighting, texgen, stencil, polygon offset, `glGet*` complete. Done when
   Quake III Arena (if the user has it) plays.
5. **The rest of GL 1.2 and AGL.** Display lists, pixel paths
   (`glDrawPixels`, `glBitmap`, `glCopyPixels`, `glCopyTexImage*`),
   3D textures, selection and feedback, `aglUseFont`, offscreen and full
   screen drawables, GLU on the host, S3TC and paletted textures.
6. **Performance:** direct presentation for full-screen contexts; reading
   back only what changed; transform in a vertex shader; bigger batches.

## Tests

- **`tests/test_opengl.c`**, the host side driven with command streams as
  `test_rave.c` drives RAVE:
  - exact: the state machine (`glGet*` after sequences of calls), error
    generation and order, display lists replaying as immediate mode,
    matrix stacks against double-precision references, clipping of
    triangles crossing each plane, pixel unpacking and conversion for
    every format and type from big-endian input, the recording format
    (stubs' output decoded back).
  - against each other: scenes through `raster.c` and `gpu.c` within the
    tolerances `test_gpu.c` uses (edges only).
  - against Mesa (Linux CI, compatibility profile, llvmpipe): the same
    streams drawn by Mesa directly; differences within tolerance or listed
    (conformance-style: primitive coverage rules, depth, blending,
    texture environments, fog, lighting), in the spirit of the OpenGL
    conformance tests and Mesa's piglit.
- **The stubs:** each generated stub run in the interpreter and the JIT
  (`ppcasm.h` and the test harness), checking what it records for given
  registers, including stack and float arguments.
- **In the guest:** the generated test application in an instruction-time
  run, screenshots byte for byte the same across runs and across x86_64
  and AArch64 (CI); and the same with `SYSHM_GPU=1` within tolerance.
- **Determinism:** a scripted UT OpenGL session in instruction time twice,
  identical screenshots and instruction counts.
- **By hand:** UT's OpenGL build in a window and full screen, resolution
  changes, Restart in the middle, two OpenGL programs at once,
  `SYSHM_GPU_FAIL` falling back to software.

## Risks

- **Size and fidelity of fixed function.** The largest risk: bugs only a
  specific game shows. The Mesa oracle and game-driven milestones keep it
  in check.
- **The Code Fragment Manager and two libraries of the same name** on 9.x:
  disabling Apple's at install avoids depending on its rules.
- **Programs that bypass the API:** calling through the context's table
  (handled by the same layout), linking to OpenGLEngine's `gli` functions
  directly, or poking at Apple's private context fields (not handled).
- **Synchronous calls in loops:** a game calling `glGetError` or
  `glGetIntegerv` per draw pays an exit each time, not a full sync
  (state is on the emulation thread); `glReadPixels` every frame waits for
  the GPU.
- **Windowed OpenGL under overlapping windows:** needs region clipping on
  every frame, and windows moving without `aglUpdateContext`.
- **macOS dropping OpenGL:** shared with RAVE; software drawing goes on,
  a Metal `gpu.c` would follow.
- **Floating-point reproducibility across hosts:** the front end must not
  let compilers contract or reorder float operations (and `raster.c`,
  which doesn't pin `FP_CONTRACT` today, should be checked the same way).
- **Memory:** UT's notes ask 40-80 MB more for OpenGL with Apple's
  implementation; syshm's libraries keep textures on the host, so the
  guest needs less, but games size their partitions by their own rules.

## Open questions

- Identity: Rage 128, Radeon, or syshm's own strings? (User.)
- Which of two same-named fragments in Extensions CFM picks, and whether
  disabling Apple's is enough (test on a 9.1 install; installing 9.1 onto
  a disk image isn't done yet in syshm).
- Did a Mac OS 9 `aglMacro.h` exist, and does any target call through
  `glc_cc` or the context's table?
- Which games use `aglSetFullScreen` rather than DrawSprocket, and what
  Apple's did with the menu bar and other applications' windows.
- Which folder Apple's engine searches for renderers (the `FindFolder`
  argument wasn't checked): only matters for option (b).
- Whether to offer Apple's own OpenGL on 9.x later through the RAVE engine
  (option (c)), for programs syshm's libraries get wrong.

## Size

Rough, with tests:

| Part | Lines |
|---|---|
| PEF builder growth, signature table, stub generator, decoder | 2,000 |
| GL state, queries, errors, display lists, textures, pixel conversion | 4,000 |
| Front end (vertices, transform, lighting, texgen, clipping, raster ops) | 2,500 |
| Backend growth in `raster.c` and `gpu.c` (shared with Glide) | 2,000 |
| AGL (formats, renderer info, contexts, drawables, regions, swaps, fonts) | 1,500 |
| GLU and OpenGLMemory on the host | 900 |
| Tests | 2,500 |

About 15,000 lines; milestone 3 (UT's OpenGL build) at about a third of
it, less what Glide built.
