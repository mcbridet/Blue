# Plan: OpenGL for Mac OS 9 games

Mac OS 9 games draw with Apple's OpenGL 1.x through AGL, its Mac binding.
This plan gives Blue OpenGL libraries of its own, generated like the other
guest tools: they record the game's calls in guest memory and hand them to
the host in batches, where Blue does OpenGL's fixed-function work and
draws with the 3D backend the RAVE engine already uses, on the GPU in real
time and in software otherwise, so instruction-time runs stay reproducible.

3dfx Glide has a plan of its own, docs/glide-plan.md, on the same backend.
It should come first (see Order).

## Status

Milestones 0 (the groundwork, shared with Glide), 1 (AGL and a clear), 2
(a triangle), 3 (textures, and Unreal Tournament's OpenGL build), 4
(arrays, fog, lighting and the rest of the common state), 5 (display
lists, pixel paths, 3D textures, OpenGLUtility), 6 (completeness:
selection and feedback, evaluators, `aglSetFullScreen`, `aglUseFont`), 7
(performance: measured, the rasterizer faster, frames shown at an even
pace) and 8 (the rest of OpenGL 1.2: logic ops, stipples, points and lines
by OpenGL's rules, dithering, the accumulation buffer) are built; Quake
III Arena's demo plays. S3TC and paletted textures are left out: Apple's
Mac OS 9.2.2 doesn't offer them for a Rage 128. What's left is under "Not
done" in milestone 8: OpenGLUtility's tessellators and NURBS, antialiased
polygons, auxiliary buffers, color index (and the imaging subset's
convolution, histogram and minmax, not planned). The user chose OpenGL
before Glide, and accepted the proposed decisions below for now
(replacing the libraries, recording in guest memory, fixed function in C
on the host, frames read back into guest memory, a Rage 128's identity).

Milestone 8:

- **Logic ops** (`glLogicOp`, `GL_COLOR_LOGIC_OP`): the 16 operations on
  the fragment's color bits and the target's, in place of blending while
  on (OpenGL 1.2's 4.1.10), through the color mask; `GL_COPY` is drawn as
  no blending. On the GPU, OpenGL's own logic op (macOS's OpenGL 4.1 on
  Metal does it: checked on an M5, each operation's bytes as OpenGL has
  them). `GL_INDEX_LOGIC_OP` is kept but means nothing: there's no color
  index drawing (Apple's renderers offered RGBA on 16- and 32-bit screens,
  and Blue no 8-bit ones). `GL_LOGIC_OP_MODE`, and `GL_COLOR_BUFFER_BIT`'s
  push and pop, have the mode.
- **Line stipple** (`glLineStipple`, `GL_LINE_STIPPLE`): fragment k of a
  line is drawn if bit (s + k) / factor % 16 of the pattern is set; the
  counter s goes on from segment to segment of a strip or loop (the
  fragments drawn, OpenGL 1.2's 3.4.2) and starts again at each separate
  line, each `glBegin`, and each polygon's outline (as feedback's
  `GL_LINE_RESET_TOKEN`); wide lines stipple their columns or rows
  together; smooth ones by where along the major axis a pixel's center is.
- **Polygon stipple** (`glPolygonStipple`, `glGetPolygonStipple`,
  `GL_POLYGON_STIPPLE`): 32 x 32 bits unpacked (and packed) through the
  pixel store as `glBitmap`'s (lists keep the unpacking), a pixel drawn if
  its bit, by window x and y from the window's bottom left, is set. On the
  GPU a uniform of 32 words and a discard.
- **Points and lines by OpenGL's rules** (OpenGL 1.2's 3.3 and 3.4;
  `raster_gl_fragments()` in `src/raster.c`, which the GPU backend shares,
  drawing each fragment as a pixel's square): aliased lines by the
  diamond-exit rule (a pixel for each diamond the segment leaves, the last
  if the end is outside it; ties broken as OpenGL's perturbed endpoints
  break them), wide ones as columns (x major) or rows of the width
  rounded, centered as 3.4.2 has it; points as squares of the size rounded,
  centered on a pixel (odd) or a pixel corner (even); sizes and widths 1
  to 64 (`GL_POINT_SIZE_RANGE`, `GL_LINE_WIDTH_RANGE`). Each fragment's
  values are where along the line its center is. RAVE's lines and points
  are drawn as before (a square of the width a pixel step), its pixels
  unchanged. Against Mesa's llvmpipe (a probe of single lines and points:
  ends inside and outside diamonds, on pixel edges, every direction, wide
  ones, sizes 1 to 4): the same pixels, but at one tie: where an x-major
  line going up crosses a pixel row's edge exactly at a column's center,
  llvmpipe takes the row below, and OpenGL's perturbed endpoints (and
  Blue) the row above.
- **Antialiased points and lines** (`GL_POINT_SMOOTH`, `GL_LINE_SMOOTH`):
  each fragment's alpha times its coverage, the area of the pixel that a
  disk of the size (sampled 16 x 16 a pixel) or a rectangle of the width
  along the line (exact: the pixel's square clipped to it) covers; with
  blending, the usual antialiasing. The hints (`GL_POINT_SMOOTH_HINT`,
  `GL_LINE_SMOOTH_HINT`) change nothing, as with Apple's ATI renderer:
  9.2.2's (disassembled) sets RAVE's antialiasing to its best
  (`kQATag_Antialias`, `kQAAntiAlias_Best`) for smooth points or lines
  whatever the hint, and to off otherwise. Mesa's llvmpipe draws smooth
  lines with alpha ramped by distance and smooth points as round aliased
  ones, so its pixels differ there (below).
- **Textured points and lines:** as triangles are, the coordinates of each
  fragment where along the line it is (divided by their q, if a vertex's q
  isn't 1); a line's level of detail is the texels along it a pixel (its ρ:
  the texture's change from end to end over its length), points
  magnified. On the GPU each fragment's square has its coordinates
  changing across it as along the line, so the GPU's level of detail is
  the same.
- **Dithering** (`GL_DITHER`, on by default): a 16-bit drawable's frames
  are dithered as they're written into the guest's memory, by a 4 x 4
  ordered (Bayer) matrix by the pixel's place in the destination: each
  component's 5 bits are v x 31 / 255 + (2t + 1) / 32 rounded down, t the
  matrix's 0 to 15 (a gray of 128 gives 9 of 16 pixels 16 and 7 15). Off,
  the top 5 bits as before. The Rage 128's own matrix couldn't be found
  (its register reference isn't to hand); a 4 x 4 ordered dither is what
  chips of its generation did. 32-bit drawables, and RAVE's frames, aren't
  dithered. A table makes it cheap: a 640 x 480 frame's 16-bit pixels in
  0.13 ms dithered, 0.05 ms not (an M5).
- **The accumulation buffer, as the generic renderer's:** Apple's 9.2.2
  renderers (their `gldGetRendererInfo`, disassembled; see "What Mac OS 9
  ships") had it only in the generic software renderer (RGB 888, ARGB
  8888, RGB 16-16-16, ARGB 16-16-16-16), ATI's none. So in Blue a pixel
  format asking for one (`AGL_ACCUM_*_SIZE`) is the generic renderer's, as
  on a Mac of the time: renderer ID 0x00020200, not accelerated (so games
  that ask for `AGL_ACCELERATED`, ATI's renderer ID or a full screen don't
  get one, as they didn't there), 8 or 16 bits a component (alpha only if
  asked for). Its contexts say vendor "Apple" and renderer "Generic"
  (Apple's strings) but Blue's version and extensions, since Blue draws
  them as it draws ATI's, on the GPU when it's on. `aglQueryRendererInfo`
  lists both, ATI's first (the order Apple's engine gave isn't known), each
  described as 9.2.2's renderers describe themselves (the generic one:
  window, offscreen, robust, backing store, MP safe, multiscreen,
  compliant, not accelerated or full screen; accumulation modes as above)
  but for what Blue doesn't have (auxiliary buffers) or draws more of (ATI's
  stencil and offscreen drawables), their colors and depths as Blue's.
  `glAccum` (`GL_ACCUM`, `GL_LOAD`, `GL_RETURN`, `GL_MULT`, `GL_ADD`),
  `glClearAccum` and `glClear`'s `GL_ACCUM_BUFFER_BIT` work within the
  scissor box, on a buffer on the host (each component in [-1, 1], to the
  bits asked for), reading the frame's colors (its drawing done first);
  `GL_RETURN` draws as a pixel rectangle through the color mask alone (as
  4.2.4 has it). `GL_ACCUM_*_BITS`, `GL_ACCUM_CLEAR_VALUE` and
  `GL_ACCUM_BUFFER_BIT`'s push and pop; without an accumulation buffer
  `glAccum` is `GL_INVALID_OPERATION`.
- **The library's data:** the generic renderer's strings moved it, so its
  version is 5: one installed by an older Blue is left alone, with the
  message to update it.
- **Checked against Mesa** (`tests/test_opengl.c`, on Linux): five more
  scenes, as milestone 4's table (pixels differing by more than 2 out of
  6,912, and the most any differs by), and a sixth not against Mesa (its
  framebuffer objects have no accumulation buffer):

  | scene | GPU | Mesa | what differs |
  |---|---|---|---|
  | logicop (seven operations over a gradient, blending on too; a line drawn twice by XOR) | 0 (1) | 0 (1) | |
  | stipple (a stippled strip, separate lines, a wide loop; stippled polygons) | 0 (1) | 34 (229) | dashes: Mesa's counter goes on by segments' lengths, not their fragments |
  | lines (aliased lines every way, ends in and out of diamonds; widths 2, 3, 5; points 1 to 5) | 0 (1) | 3 (3) | a smooth-shaded loop's colors |
  | smoothlines (antialiased lines 1 and 2.5 wide, points 7.5 and 3.2, blended) | 0 (1) | 278 (122) | llvmpipe's coverage (above) |
  | texlines (textured lines 1 and 3 wide, nearest and minified linear; points 6 across) | 0 (2) | 51 (20) | the linear line's mix: half a texel a pixel |
  | accum (two images averaged, scaled, moved; cleared and returned within scissor boxes, through a color mask) | 0 (0) | | |

  `tests/test_raster.c`: lines and points as llvmpipe draws them (first
  and last pixels, counts), the stipple's counter, coverage summing to the
  disk's and rectangle's areas, each logic op's bits with and without the
  color mask, the polygon stipple's rows; `tests/test_gpu.c`: all of those
  on the GPU against `raster.c` (none off by more than 1);
  `tests/test_glfront.c`: the stipple through `glBegin` (a strip going on,
  separate lines starting again), the polygon stipple through the pixel
  store and back, logic ops' state and errors, the accumulation buffer's
  operations, clamping, scissor, color mask and 8 bits' steps;
  `tests/test_opengl.c`: the generic renderer's pixel formats (and those
  refused), renderer info, strings and bits, and a 16-bit window's dither
  (and none, `GL_DITHER` off).
- **The test application:** scenes `lines` (a smooth-shaded quad through a
  polygon stipple and a quad over it by XOR, stippled lines, fans of lines
  from a display list aliased and antialiased, points aliased and
  antialiased, a wide line and points textured) and `accum` (a context of
  the generic renderer, "Apple" and "Generic"; a turning bar blurred by
  eight passes accumulated, a gradient loaded, scaled and a triangle
  accumulated over it). On installed 8.5 in instruction time, at 32 and 16
  bits: the same screenshots on macOS and Linux, and on the GPU (Metal)
  within 1 (16 bits: one step of 5 bits, where a dither threshold falls
  the other way). The older scenes at 32 bits are as they were but
  `lists`, whose sphere of lines is drawn by OpenGL's rules now; at 16
  bits all are dithered.
- **RAVE, the boots:** the 20 random streams of RAVE calls give the same
  hashes, the GPU's 6,000 frames of them are milestone 6's exactly, and
  the 8.5 and 9.1 CDs boot to main's screenshots.
- **Games:** Quake III's demo and UT play, but as in milestone 6 the
  library's data moved, so in instruction time they play out otherwise
  than main (Quake III 309,321 calls and 4,553 frames in 17 billion
  instructions; UT's match 31,963 calls and 734 frames, in a busier part
  of the map; the same on the GPU). Built with the old data (version 4,
  without the generic renderer's strings), this milestone's Blue gives
  main's runs exactly (305,955 calls and 4,497 frames; 24,490 and 1,542;
  UT's eight screenshots, in software and on the GPU), and Quake III's
  frame, which is 16-bit, differs only by its dither (with dithering
  off, it's main's exactly). So the host's cost could be compared on the
  same runs: that build and main's, alternately, in software with one
  rasterizer thread, by the host's instructions retired (`time -l`), since
  the machine's other work made times swing by 5-20% from run to run:
  Quake III's demo 1,073.7-1,074.0 billion against main's 1,075.2-1,075.8
  (its frames dithered now), UT's match 1,638.3-1,638.9 billion against
  1,645.5-1,646.0; their cycles within the runs' noise. The rasterizer
  alone (six of the RAVE streams, one thread): 94.4 billion instructions
  against 94.9, and 2-4% fewer cycles. `BLUE_GL_PROFILE`, best of three
  runs each, in milliseconds a frame: Quake III 4.14 main, 4.38 here
  (waiting for the backend 2.81 and 2.97, presenting 0.19 and 0.23: the
  dither), UT 22.66 and 21.94 (11.48 and 11.22), as the host's load rose
  through the runs.
- **Not done:** OpenGLUtility's tessellators and NURBS (their objects and
  properties only: until a program needs them); antialiased polygons
  (`GL_POLYGON_SMOOTH` is kept but not drawn); auxiliary buffers (the generic renderer had 4;
  Blue's pixel formats refuse `AGL_AUX_BUFFERS`); color index (no index
  pixel formats); the imaging subset's convolution, histogram and minmax
  (not planned).

Milestone 7:

- **Measuring** (`BLUE_GL_PROFILE=FILE`, in `src/opengl.c`; `src/nk.c`,
  `src/surface.c` and `src/gpu.c` count their parts): a line for each
  frame shown on the screen with the host time since the one before and
  where it went: the libraries' host calls (the front end: everything in
  them but what follows), waiting for the backend (to take a batch, to
  finish), reading back from the GPU, writing the frame into guest memory,
  napping until the frame is due (pacing) or idle; the rest is the guest
  (translated code, devices). Then the calls, records and primitives, the
  GPU thread's time drawing and reading back, the textures it made; and a
  line at each update of the window. Real-time runs on the shared host
  were noisy (other work on it, and in GPU runs macOS clocking the
  processor and GPU down, as Blue sleeps more than half of each frame),
  so the rasterizer was measured by replaying batches captured from the
  games (a scratch tool, not kept: every frame drawn again and checked
  against the captured one's hash, best of three) and counting
  instructions (callgrind on Linux), and the games in instruction time.
- **Where the time goes**, real time on an M5 (4 performance cores), 640 x
  480, in milliseconds a frame (Quake III's `demo001`; UT's intro, and a
  deathmatch against bots):

  | | fps | frame p50 / p90 / p99 | guest | front end | waiting for the backend | reading back | presenting | napping (paced) |
  |---|---|---|---|---|---|---|---|---|
  | Q3, GPU | 60.0 | 16.7 / 17.8 / 18.9 | 3.1 | 1.8 | 0.1 | 1.6 | 0.3 | 9.7 |
  | Q3, software | 59.2 | 16.7 / 17.6 / 24.2 | 1.5 | 0.9 | 6.8 | | 0.2 | 7.5 |
  | UT intro, GPU | 60.0 | 16.7 / 17.3 / 18.5 | 4.5 | 0.8 | 0.5 | 1.9 | 0.4 | 8.6 |
  | UT intro, software | 52.3 | 18.5 / 23.2 / 26.9 | 3.4 | 0.5 | 12.5 | | 0.3 | 2.5 |
  | UT match, GPU | 60.0 | 16.7 / 18.4 / 21.6 | 3.8 | 0.3 | 0.2 | 2.3 | 0.4 | 9.7 |
  | UT match, software | 59.1 | 16.7 / 17.5 / 21.8 | 1.4 | 0.2 | 9.6 | | 0.3 | 5.6 |

  Quake III makes about 670 calls, 1,040 records and 3,400 primitives a
  frame; UT's match 5 calls (lists and arrays recorded) and 6,000-9,000
  records for 800-1,000 primitives; UT's intro makes 58 textures a frame
  (275 KB, its animated ones). The guest library's own code (recording)
  is a small part of the guest's time: about 15 PowerPC instructions a
  record, some 0.3 ms a frame in UT and less in Quake III. On the GPU
  every frame is ready in time, with 7-10 ms to spare: reading back is
  the biggest of Blue's own costs, but it's mostly the GPU finishing the
  frame at the low clocks macOS gives it between frames (replayed at full
  clocks a whole frame, drawn and read back, takes 0.5-0.9 ms). In
  software the rasterizer is the cost: inside it, unpacking pixels to
  floats, the blend factors, texel addresses, packing, the stages. The
  frame times above are when each frame is swapped, which varies with the
  guest's work; what the window shows is below.
- **The rasterizer** (`src/raster.c`), its pixels the same bit for bit:
  pixels unpacked and packed as whole vectors (each lane the same IEEE
  operation as one at a time; NaN packs to 0, as converting it one at a
  time gives on the hosts Blue runs on), the blend factors as vectors,
  blending by one and zero (adding) skipped (the color as it is, but -0
  for +0, which packs the same), bilinear texel addresses without the
  float clamping and floor when the coordinates are below 2^24 (then they
  and the next ones are exact whole numbers). Replayed, every frame as
  captured: UT's frames 30% faster in one thread (40-42 to 29 ms a frame;
  51 to 27 under another load) and with the default threads (24-26 to
  17.5-18.3 ms), Quake III's demo 26% (9.5 to 7.0 ms). In real time,
  software: Quake III 59.2 to 59.9 frames a second (waiting for the
  backend 6.8 to 5.1 ms a frame, p99 24.2 to 21.2 ms), UT's intro 52.3 to
  56.8 (12.5 to 8.9 ms); in instruction time UT's match waits 6.7-10.2 ms
  a frame rather than 11.6-14.5. The same 20 random streams of RAVE calls
  give the same hashes (macOS, Linux on ARM and on x86-64), the GPU's
  frames of them are milestone 6's exactly, and the games' instruction-time
  runs are milestone 6's (Quake III's frame on macOS, Linux and the GPU;
  UT's eight screenshots).
- **Pacing the window** (`src/nk.c`): in real time the window shows each
  paced frame (OpenGL's or RAVE's) when it's due, rather than at its own
  next update, which ran a little slower than 60 a second, so that every
  few seconds a frame was shown twice and the next never. Quake III in a
  window (SDL's dummy video driver): updates every 16.70 ms (p99 17.69),
  0.8% of frames never shown, before; every 16.67 ms (p99 16.91), none
  lost, after; UT's match, 99.9% of updates a new frame. In instruction
  time nothing changes.
- **Tried and not kept:** a `glFlush` after each GPU draw job (no clear
  change); four helper threads for the rasterizer rather than two (faster
  replayed at times, but no clear gain in the games' runs, with a fifth
  busy thread on a host of four performance cores); bands
  of 8 rows rather than 16 (the same); setting triangles up once a batch
  rather than once a band (a band's setup costs nothing measurable). The
  plan's other ideas weren't the costs measured: presenting directly
  (writing a frame to guest memory is 0.2-0.5 ms), reading back only what
  changed (the games redraw every pixel each frame), transform in a vertex
  shader (the front end is 0.2-1.8 ms a frame, and software needs it in
  C), bigger batches (waiting for the GPU is 0.1-0.6 ms a frame). Left as
  candidates: reading back asynchronously while the guest waits for its
  frame's turn (for games short of time), and a vertex cache in
  `glDrawElements` (about 0.25 ms a frame in Quake III).
- **Tests:** without EGL (a Linux without libEGL), `opengl` crashed:
  feedback against Mesa ran without Mesa; it's skipped now, as the rest
  is. On AMD's radeonsi (Mesa 26.2, a Radeon RX 6800 XT, x86-64 Linux) two
  checks failed: `gpu`'s projective square had 14 pixel centers on texel
  edges (exactly, by its numbers: where the division's rounding decides,
  and radeonsi rounds otherwise at 14 of them, llvmpipe at 2), and
  `opengl`'s stencil scene (11 pixels where the alpha tested against 0.5
  was exactly 0.5) and pixels scene (one depth image's depth exactly a
  quad's: 6 pixels) had ties too. The scenes and the square are
  moved off them (each texel edge now at least a thousandth of a texel
  from a pixel center; the alpha's 0.5 between pixel centers; the depths
  0.004 off the quad's), and `gpu`'s texture's neighbors are 16 apart, so
  a pixel a texel off shows: 0 pixels differ now on radeonsi, llvmpipe and
  Metal. The pixels scene's `glCopyPixels` of depths is radeonsi's own bug
  (no color at all, where OpenGL has the raster color, and the depths
  written with the depth test off; llvmpipe does as OpenGL says): those
  288 pixels aren't compared on radeonsi, with a message. Against
  radeonsi then, by scene (pixels differing by more than 2; covered by one
  only): gradient 0, depth 1 (1, a center on an edge: snapping), flat 0,
  smooth 2,739 (quads split otherwise, as llvmpipe), primitives 0, blend 0,
  texture 0, clamp 3,840 (`GL_CLAMP`'s border, as llvmpipe), edge 0,
  multitexture 0, lighting 0, specular 0, fog 0, texgen 0, arrays 0,
  stencil 0, offset 0, clip 0, lists 0, pixels 0, texture3d 0, glu 31 (30;
  libGLU's quadrics), eval 0, eval_lit 0; and our GPU backend on radeonsi
  against our software: 0 in each but depth (1).

Milestone 6:

- **Selection and feedback** (`src/glselect.c`): `glRenderMode`,
  `glSelectBuffer`, `glFeedbackBuffer` (each type, 2D to 4D with colors
  and texture coordinates), the name stack (64 deep), `glPassThrough`, the
  queries and buffer pointers. The front end clips primitives to the view
  volume (not the guard band) and, instead of drawing them, makes hit
  records (depths as 0 to 2^32 - 1) or feedback tokens: polygons as
  triangles in Mesa's order (a polygon, or a clipped one, as (v[i],
  v[i + 1], v[0])), outlined ones' edges and points, lines with
  `GL_LINE_RESET_TOKEN` where the stipple is reset, the raster position's
  bitmap, draw and copy tokens (bitmaps still move it). Nothing is drawn or
  cleared meanwhile. The records stay on the host until `glRenderMode`
  leaves the mode, then go into the program's buffer (what overflowed it
  counted: -1).
- **Evaluators** (`src/gleval.c`): `glMap1` and `glMap2` of each kind,
  float and double, with any strides (lists keep their control points),
  `glMapGrid`, `glEvalCoord`, `glEvalPoint` and `glEvalMesh` as OpenGL
  1.2's 5.1 has them (filled meshes as quad strips), orders to 8,
  rational vertices, `GL_AUTO_NORMAL` (the normal of the point divided by
  w, normalized), `glGetMap`, `GL_EVAL_BIT`. Like Mesa, a vertex's
  evaluated color, normal and texture coordinates don't become the current
  ones, and maps are unit 0's (`glMap` with another unit active is
  `GL_INVALID_OPERATION`).
- **`aglSetFullScreen`**: a context of a full screen pixel format draws
  to its device's whole screen. Asked for another size (0 x 0: the
  screen's as it is) or the screen at another depth than the pixel
  format's, the library switches the display with the Display Manager
  (`DMSetDisplayMode`, by a callout) to the mode of exactly that size, as
  the AGL reference has it (else `AGL_BAD_FULLSCREEN`); the viewport
  becomes the screen's, as Apple's 9.2.2 does. The mode it was in comes
  back when that context's drawable goes (another drawable, none, the
  context destroyed, the process quitting). Frames go to the screen
  unclipped, paced by its refresh as a window's. The menu bar and other
  windows are left as they are (they may draw over it, as on a Mac).
- **`aglUseFont`**, as Apple's 9.2.2 does it (disassembled): QuickDraw
  draws each glyph (the font, face and size set in a 1-bit offscreen
  world, measured by `GetFontInfo`), by callouts; the host turns its bits
  into list base + i, a `glBitmap` of (CharWidth + 2e) x (ascent +
  descent + 2e) from (e, descent + e - 1), moving CharWidth on, where e
  is half the size for a styled face (else 0). Errors as Apple's: a list
  being made, `AGL_BAD_STATE`; no such font, `AGL_BAD_VALUE`; no memory,
  `AGL_BAD_ALLOC`.
- **The library's imports:** OpenGLLibrary imports what those call from
  InterfaceLib (`DMSetDisplayMode`, `RealFont`, `NewGWorld` and the like,
  all in Apple's import list but the first). That moved its data, so its
  version went to 3 (and to 4 when the version string became "1.2 Blue"):
  one installed by an older version of Blue is left alone (every call
  fails, and Blue says to update it), not misread.
- **S3TC and paletted textures:** left out. In Mac OS 9.2.2,
  OpenGLEngine, OpenGLRendererATI and the software renderer list neither
  `GL_EXT_texture_compression_s3tc` (or `GL_S3_s3tc`) nor
  `GL_EXT_paletted_texture`; only the "NVIDIA OpenGL" engine on that disk
  (for GeForce cards) does.
- **Checked against Mesa** (`tests/test_opengl.c`, on Linux): two more
  scenes, and selection's and feedback's records:

  | scene | GPU | Mesa |
  |---|---|---|
  | eval (a curve's points by a color map, a rational curve, a patch colored and textured by maps, a list's map) | 0 (2) | 0 (2) |
  | eval_lit (a bumpy patch's made normals in triangle strips of grid points, a rational cylinder's, a flat patch's mapped normals) | 0 (0) | 0 (1) |

  Six feedback and selection scenes (window coordinates and perspective
  of every kind of primitive, clipped, outlined, culled; the raster
  position's tokens; a buffer overflowed; the name stack's hits; two of
  evaluators: curves', patches' points and lines with their colors and
  texture coordinates, lit ones' colors) give Mesa's records, within
  1e-3. Where Mesa departs from OpenGL 1.2 (see Findings in plan.md), the
  scenes avoid it and `tests/test_glfront.c` checks OpenGL's way: line
  reset tokens, flat shading's colors in feedback, the current values after
  evaluation, `glGetMap`'s order, a backwards domain's normals, filled
  meshes' quads; and the errors, defaults, grids and `GL_EVAL_BIT`.
  `tests/test_opengl.c` runs `aglSetFullScreen` and `aglUseFont` with the
  Toolbox's calls answered by fakes (mode switches and their failures, the
  mode back when the drawable or the process goes; glyphs drawn by
  `glCallLists` against the fake font's, plain and styled; the errors).
- **The test application:** scene `fullscreen` (a full screen context,
  640 x 480 from 800 x 600, the triangle scene drawn on the whole screen)
  and `font` (Times 24 and Geneva 18 bold through `aglUseFont`, a line of
  each by `glCallLists`). On installed 8.5 in instruction time: the same
  screenshots on macOS and Linux, and on the GPU (Metal) within 2 of
  them; the older scenes' screenshots are as they were.
- **Games:** Quake III Arena's demo and Unreal Tournament's OpenGL build
  play as before, but the library's new imports (the Code Fragment
  Manager resolving them, its data moved) change how many instructions
  the guest spends before the games start, so in instruction time they
  play out differently from main: Quake III's `demo001` is at another
  moment 17 billion instructions in (305,904 calls and 4,500 frames, the
  same frame on macOS and Linux, on the GPU the same calls and frames),
  and UT's match spawns elsewhere (34,142 calls, 1,742 frames). Built
  without the imports (the old layout), this milestone's Blue gives
  main's runs exactly: Quake III's frame (306,893 calls, 4,511 frames, on
  the GPU too), UT's eight screenshots, 34,035 calls and 1,547 frames.
  The 3D engine's pixels are main's (software and GPU), and the 8.5 and
  9.1 CDs boot to main's screenshots.
- Not yet: OpenGLUtility's tessellators and NURBS (their objects and
  properties only: until a program needs them), the accumulation buffer
  (pixel formats asking for one are refused), logic ops; line and polygon
  stipples are kept but not drawn; lines and points aren't textured; smooth
  points and lines are drawn as aliased ones.

Milestone 5:

- **Display lists** (`src/gllist.c`, `src/opengl.c`): `glNewList` and
  `glEndList` (compile, and compile and execute), `glCallList` and
  `glCallLists` (every type of name, with `glListBase`), `glGenLists`,
  `glDeleteLists`, `glIsList`; lists call lists 64 deep, and are shared
  as textures are. A list keeps what OpenGL 1.2 says it keeps (queries,
  client state and the like are done there and then), each call with the
  bytes its pointer pointed to and, for images, the pixel store as it was;
  `glDrawArrays` and the other array draws are kept as the `glBegin`,
  attribute calls and `glEnd` they make, so later changes to the arrays
  don't show. Every scene against Mesa (below) made into a list and called
  gives the same pixels as drawn directly, in software and on the GPU.
- **The raster position and pixels** (`src/glpixel.c`): `glRasterPos`
  (a vertex's transform and clipping; its color, texture coordinates and
  distance), `glBitmap` (moving the raster position), `glDrawPixels` and
  `glCopyPixels` of colors, depth and stencil (color indices through the
  maps), zoomed by any amount (backwards too), `glPixelTransfer` (scales,
  biases, the index shift and offset, `GL_MAP_COLOR` and
  `GL_MAP_STENCIL`), `glPixelMap` (the ten maps, 256 entries each),
  `glReadPixels` of depth and stencil, `glCopyTexImage1D` and `2D`,
  `glCopyTexSubImage1D`, `2D` and `3D`, `glGetTexImage`. Both backends
  draw a pixel rectangle as fragments through the per-fragment operations
  (`RASTER_CMD_PIXELS`), without texturing, fog or the color sum; the GPU's
  depth and stencil are read back when they're read or copied. Images go
  from the raster position rounded to the nearest pixel corner, as Mesa
  does (see Findings in plan.md for where Mesa departs from OpenGL 1.2's
  rectangles).
- **3D textures:** `glTexImage3D`, `glTexSubImage3D`, `GL_TEXTURE_3D`
  (before 2D and 1D in a unit), the r coordinate's wrap, the image height
  and skip images pixel stores, up to 128 texels a side; `raster.c` samples
  them trilinearly between the two slices nearest r (its level by s and t,
  as for 2D), `gpu.c` with a `sampler3D`.
- **OpenGLUtility** (`src/glu.c`): the matrices (`gluOrtho2D`,
  `gluPerspective`, `gluLookAt`, `gluPickMatrix`), `gluProject` and
  `gluUnProject`, `gluScaleImage`, `gluBuild1DMipmaps` and
  `gluBuild2DMipmaps`, and the quadrics (spheres, cylinders, disks and
  partial disks in each style, with flat or smooth normals, either way out,
  textured), done on the host as SGI's GLU does them, by gl calls to the
  current context (so lists keep them). Its quadrics' calls are SGI's call
  for call (compared with Mesa's libGLU, SGI's code), but where SGI's reads
  normals it never set (flat ones for points and silhouettes): there the
  smooth ones. Images are scaled through 16 bits a component, as SGI's GLU
  1.2 does (Apple's is "1.2 MacOS 8.0"): Mesa's libGLU (SGI's 1.3) makes 2D
  mipmaps from 8-bit images by another filter, within a level of these when
  the image needs no scaling. Its strings are Apple's (its version, its
  extensions, `gluErrorString`'s). Quadrics, tessellators and NURBS
  renderers are blocks from the program's Memory Manager (`NewPtrClear`, by
  a callout); quadrics' errors go to their callbacks. Tessellators and
  NURBS renderers keep their properties but draw nothing yet. Like Apple's,
  it imports OpenGLLibrary's `glc_cc` (as data: the fragment builder's
  data imports are new) to find the process's current context.
- **Checked against Mesa** (`tests/test_opengl.c`, on Linux), four more
  scenes, as milestone 4's table:

  | scene | GPU | Mesa |
  |---|---|---|
  | lists (lists calling lists, one redefined, compile and execute, arrays and an image as they were, `glCallLists`' types from a list base) | 0 (1) | 0 (1) |
  | pixels (each kind of image, zoomed, the transfer, bitmaps, copies of colors and depths, a stencil image) | 0 (1) | 0 (1) |
  | texture3d (nearest and trilinear, r clamped and repeated, r generated, a slice replaced) | 0 (1) | 0 (1) |
  | glu (perspective and viewpoint, quadrics of each kind and style, mipmaps' first and third levels) | 0 (2) | 31 (229): lines |

  In "glu", Mesa's lines at any slope step to the next pixel at other
  points than ours (30 pixels drawn by one only); its filled quadrics are
  drawn alike. "glu" is compared when `BLUE_TEST_GLU` names Mesa's
  libGLU (`libGLU.so.1`). Unit tests (`tests/test_glfront.c`,
  `tests/test_opengl.c`) check what Mesa can't: OpenGL 1.2's rectangles
  for fractional and negative zooms, pixel maps (Mesa 25.2's corrupt its
  heap: see Findings in plan.md; that was milestone 5's intermittent crash
  in the tests), color indices, depth and stencil images read and drawn,
  OpenGLUtility's matrices, projections, images, objects, strings and
  errors.
- **The test application** imports OpenGLUtility now. Scene `lists`: a
  sphere, a sphere as lines and a ring under a call to the first made into
  lists through OpenGLUtility (`gluPerspective`, `gluLookAt`, quadrics),
  called lit in three colors and plain; scene `pixels`: in `gluOrtho2D`'s
  window coordinates, an image drawn four times its size, a glyph by
  `glBitmap` four times along, the image copied half its size the other
  way up. On installed 8.5 in instruction time: the same screenshots on
  macOS and Linux; on the GPU (Metal), `pixels` is the same and in `lists`
  1,302 pixels differ, by 1. `clear`'s, `triangle`'s and `lit`'s are as
  they were.
- **Quake III Arena's demo** (id's Mac demo of December 1999, from
  archive.org's mirror of id's FTP site; kept out of the tree): its
  installer (Installer VISE) installs it on a clone of the 8.6 disk with
  Blue's libraries, and it plays: the menus, and `demo001` in q3dm7 with
  its lightmaps, models, effects and HUD (scripted in instruction time:
  the Finder opens it, the menus are driven by keys and mouse motion). It
  calls no gl function Blue doesn't do. With `TZ=UTC` the same frame
  comes out on macOS and Linux 17 billion instructions in; on the GPU,
  the same calls and frames (306,893 and 4,511 by then), and pixels mostly
  within a step of the 16-bit screen (200 differ by more, at edges and in
  an explosion: mipmap levels chosen per pixel there, per triangle in
  `raster.c`).
- **Unreal Tournament's OpenGL build** still plays: in instruction time
  the same calls and frames as before, and its eight screenshots the same
  as main's.
- **Done when** display lists, pixel paths, 3D textures and OpenGLUtility
  are checked by scenes against Mesa, the test application and UT, and
  Quake III's demo plays (what this milestone was given): as above.
  Apple's OpenGL SDK samples (GLUT demos for Mac OS 9) weren't found to
  download (archive.org has none; the Wayback Machine, where Apple's pages
  are, wasn't reachable).
- Not yet: selection and feedback, evaluators, `aglUseFont`,
  `aglSetFullScreen`, S3TC and paletted textures, OpenGLUtility's
  tessellators and NURBS (their objects and properties only), the
  accumulation buffer (pixel formats asking for one are refused, as
  before), logic ops; line and polygon stipples are kept but not drawn;
  lines and points aren't textured; smooth points and lines are drawn as
  aliased ones.

Milestone 4:

- **Lighting** (`src/gllight.c`), per vertex in eye coordinates, as
  OpenGL 1.2's 2.13.1 has it (the Rage 128 had no transform engine: its
  OpenGL lit on the processor too): 8 lights (directional and positional,
  attenuated, spots), front and back materials, the light model's ambient
  color, local viewer, two sides and separate specular (the secondary
  color added after texturing, as a stage of the backend's), the color
  material, normals by the modelview's inverse transpose with
  `GL_NORMALIZE` and `GL_RESCALE_NORMAL`. Powers are libm's in doubles,
  rounded to floats; the spot's cosine is the front end's own. Positions
  and spot directions are kept in eye coordinates as they were given.
- **Fog** (linear, exp, exp2) by the eye-space |z| of each vertex, divided
  by w and interpolated, the factor per pixel (the backend's). Mac OS 9 has
  no fog coordinates: its OpenGLLibrary exports no `glFogCoord` (nor
  `glFogCoordPointer`), and Apple's engine lists no `GL_EXT_fog_coord`, so
  `GL_FOG_COORDINATE_SOURCE` is an invalid enum here as there.
- **Texture coordinate generation:** object and eye linear (eye planes
  as given, in eye coordinates), the sphere map, and
  `GL_NV_texgen_reflection`'s normal and reflection maps, before the
  texture matrix. **User clip planes** (6): distances in eye coordinates,
  clipped exactly (not through the guard band).
- **Client arrays** (`src/glarray.c`): vertex, normal, color, secondary
  color, edge flag, index and both units' texture coordinates, of every
  type OpenGL 1.2 takes for each (big-endian in the program's memory;
  colors and normals normalized as table 2.6 says), `glInterleavedArrays`'
  14 formats, `glArrayElement`, `glDrawArrays`, `glDrawElements` and
  `glDrawRangeElements` (indices of 1, 2 or 4 bytes), compiled arrays
  (`glLockArraysEXT`: the same arrays). They're drawn as `glArrayElement`
  between `glBegin` and `glEnd` would be. `opengl.c` checks the bytes the
  elements take before the front end reads them (indices are the
  program's, so an element outside them leaves its attribute alone, never
  reading past what was checked).
- **Stencil** in both backends: `raster.c` keeps a stencil buffer (each
  surface has one) and, with the test on, takes a fragment through
  OpenGL's order (alpha test, stencil, depth, then the operation for
  each outcome: keep, zero, replace, increment and decrement, saturating
  or wrapping as `GL_EXT_stencil_wrap`'s, invert), with the value and
  write masks; clears of it. RAVE's states never have it, and its path is
  as it was (the same pixels; within 2.5% of the time on 3,000 60-pixel
  triangles, and none slower on 100,000 small ones). `gpu.c`'s targets
  are `GL_DEPTH32F_STENCIL8` now, with stencil state per batch (RAVE's GPU
  frames in the random streams are the same bytes).
- **Polygon offset** (fill, line, point: the polygon's largest depth slope
  times the factor plus the units times the drawable's depth resolution,
  16 or 24 bits) and **polygon modes** (points and lines for each face:
  the polygon's edges, not the diagonals its triangles share, edge flags
  honored, edges clipping made not drawn; a `GL_POLYGON`'s closing edge at
  `glEnd`), done in the front end.
- **`glPushAttrib` and `glPopAttrib`** (`src/glattrib.c`, 16 deep), every
  group of OpenGL 1.2's tables (texture bindings and the bound textures'
  parameters put back through `glBindTexture` and `glTexParameter`);
  `glPushClientAttrib` and `glPopClientAttrib` (the pixel stores, the
  arrays).
- **Queries:** `glGet*` for all the state above and the rest of OpenGL
  1.2's that's kept (hints, stipples, limits; the raster position and
  pixel transfers as they start, until milestone 5), `glGetLight`,
  `glGetMaterial`, `glGetTexGen`, `glGetClipPlane`, `glGetPointerv`,
  `glGetPolygonStipple`, `glIsEnabled` of the arrays.
- **Extensions:** now also `GL_APPLE_transform_hint` (the hint, taken),
  `GL_EXT_rescale_normal`, `GL_NV_texgen_reflection` and
  `GL_EXT_compiled_vertex_array`, in Apple's order.
- **Checked against Mesa** (`tests/test_opengl.c`, on Linux), eight more
  scenes through the libraries (vectors and arrays in guest memory). Pixels
  differing by more than 2 out of 6,912, and the most any differs by (GPU:
  `gpu.c` on llvmpipe; on Metal none differ by more than 1):

  | scene | GPU | Mesa |
  |---|---|---|
  | lighting (three kinds of light, local viewer, two sides, color material, normals rescaled) | 0 (0) | 15 (3): the back face |
  | specular (separate, over a texture; then single) | 0 (1) | 0 (1) |
  | fog (each mode, in perspective) | 0 (0) | 0 (1) |
  | texgen (object, eye, sphere, reflection) | 0 (1) | 0 (1) |
  | arrays (each type, interleaved, each index type, `glArrayElement`, a secondary color, locked) | 0 (2) | 0 (2) |
  | stencil (a shape, counting, invert with a write mask, after the alpha test, depth failing) | 0 (1) | 0 (1) |
  | offset (decals forward and back, on a floor and a wall) | 0 (1) | 0 (1) |
  | clip (two planes given under a rotation) | 0 (0) | 0 (1) |

  None differ in what's covered. (The lighting scenes are orthographic:
  in perspective Mesa's colors are corrected for it and ours are linear in
  screen space, as "smooth" shows.)
- **The test application:** scene `lit`: in perspective, a bump on a floor
  drawn by `glDrawElements` from vertex and normal arrays in its data, lit
  (a light's and the material's vectors through `glLightfv` and
  `glMaterialfv`), shiny, fogged; then a quad drawn only where a triangle
  was put in the stencil. On installed 8.5 in instruction time: the same
  screenshot on macOS and Linux; on the GPU (Metal), 44 pixels differ, by
  1. `clear`'s and `triangle`'s screenshots are as they were (the pixel
  format now asks for 8 stencil bits).
- **Tests:** `tests/test_glfront.c` (lighting's values for each kind of
  light, two sides, separate specular, the color material and the errors;
  each texture coordinate generation; clip planes and fog; polygon modes'
  lines and points, offset's depths; the stencil's raster state and
  clears; both attribute stacks; arrays' types, indices, bounds,
  interleaved formats and errors; queries), `tests/test_raster.c` (each
  stencil operation, the masks, the order of the tests, clears).
- **Unreal Tournament's OpenGL build** still plays (in instruction time
  the same calls and frames as before this milestone, about as fast).
- **Done when:** the plan said Quake III Arena plays, if the user has it;
  the user's media have no Quake III (the Quake CD is the PC one), so the
  milestone was checked by the scenes against Mesa, the test application
  in the guest and Unreal Tournament.
- Not yet: display lists, the raster position and pixel paths
  (`glBitmap`, `glDrawPixels`, `glCopyPixels`, `glCopyTexImage`), 3D
  textures, selection and feedback, evaluators, `aglUseFont`,
  OpenGLUtility (milestone 5); line and polygon stipples are kept but not
  drawn; lines and points aren't textured; smooth points and lines are
  drawn as aliased ones; logic ops.

Milestone 3:

- **Textures** (`src/gltex.c`, `src/glfront.c`): texture objects
  (`glGenTextures`, `glBindTexture`, `glDeleteTextures`, `glIsTexture`,
  `glAreTexturesResident`, `glPrioritizeTextures`) in a namespace that
  contexts made to share (`aglCreateContext`'s second argument) share;
  1D and 2D images and subimages (`glTexImage1D`/`2D`, `glTexSubImage1D`/
  `2D`, the proxies) in every format and type of OpenGL 1.2, the packed
  ones, BGR, BGRA and `GL_ABGR_EXT` included, read big-endian from guest
  memory with the unpack stores (`glPixelStore`: alignment, row length,
  skips, swapped bytes); texels kept as 8-bit ARGB whatever the internal
  format, as the base format gives them (a table in `gltex.c`); the
  parameters (filters, wraps, base and maximum level; the LOD clamps and
  priority kept) and level parameters, with their errors; completeness as
  OpenGL 1.2 has it (an incomplete texture turns its unit off); two units
  (`glActiveTextureARB`, `glClientActiveTextureARB`,
  `glMultiTexCoord*ARB`), each with its environment (`GL_REPLACE`,
  `GL_MODULATE`, `GL_DECAL`, `GL_BLEND`, `GL_ADD`) made the backend's
  stages for each base format as OpenGL 1.2's table 3.18 (and
  `GL_ARB_texture_env_add`) says; the texture matrices; q divided per
  pixel, only for primitives whose q isn't 1. The backend's copy of a
  texture is made when it's first drawn and shares the texels; a change
  retires it (its texels copied first), and it's freed once no surface is
  drawing with it. Where OpenGL leaves room, the Rage 128's way (in
  `glfront.h`): `GL_CLAMP` clamps to the edge texels, as
  `GL_CLAMP_TO_EDGE` (the chip had no texture borders), and textures keep
  8 bits a component whatever internal format is asked for.
- **`glReadPixels`** (what's recorded drawn first; every format and type,
  the pack stores), and the extensions string: Mac OS 9.2.2's engine's
  extensions in its order, less what Blue doesn't do (`GL_APPLE_packed_pixel
  GL_ARB_transpose_matrix GL_EXT_clip_volume_hint GL_ARB_multitexture
  GL_ARB_texture_env_add GL_EXT_blend_color GL_EXT_blend_minmax
  GL_EXT_blend_subtract GL_EXT_texture_env_add GL_EXT_abgr GL_EXT_bgra
  GL_SGIS_texture_edge_clamp`).
- **Swaps paced:** `aglSwapBuffers` to a window's or the screen's port
  ends a frame for the screen, paced by the display's refresh as the 3D
  engine's are (`frame_paced()` in `src/nk.c`; each context a frame source
  of its own after RAVE's): in real time the guest waits in the call, in
  instruction time guest time moves on. Offscreen swaps aren't paced. In
  instruction time UT's OpenGL and RAVE builds draw the same number of
  frames (639 by 44 billion instructions); before, the OpenGL build drew
  as fast as the host could. `BLUE_GL_FPS=1` prints the frame rate.
- **Installing** (`src/install.c`): the three libraries are guest tools
  like the others, offered with them, and put in Extensions. On Mac OS 9,
  Apple's are enabled exactly while Blue's aren't: once Blue's are all
  in Extensions, the installer moves Apple's (files named OpenGLLibrary,
  OpenGLMemory and OpenGLUtility, of type `shlb` and not Blue's creator,
  in Extensions) to Extensions (Disabled) with `PBCatMoveSync` (the folder
  made if there's none: `FindFolder`'s `extD`), and says so; if Blue's
  are gone (none of them anywhere: deleted by hand, as there's no
  uninstall) while Apple's are only in Extensions (Disabled), the offer is
  "Put Back" instead, and nothing of Blue's OpenGL is installed. Apple's
  in Extensions with Blue's already there gives "Move". The other tools'
  offers and messages are as before. On a clone of the user's 9.2.2:
  `tools: installed Blue OpenGL Library, Blue OpenGL Memory and Blue
  OpenGL Utility in "MacOS9Lives:System Folder:Extensions"; moved Apple's
  OpenGLLibrary, OpenGLMemory and OpenGLUtility to "MacOS9Lives:System
  Folder:Extensions (Disabled)". Programs opened from now on find Blue's
  OpenGL.`, and UT's OpenGL build, opened in that same session, drew with
  Blue's; with Blue's three deleted, the next start put Apple's back.
- **Unreal Tournament's OpenGL build** (the UT CD's `Extras:OpenGL
  UT.sea`: an application and `OpenGLDrv.ini`), on a clone of the 8.6 disk
  with UT installed, the libraries in Extensions and
  `GameRenderDevice=OpenGLDrv.OpenGLRenderDevice`: the intro and a
  DM-Barricade match (opened from the console) play, on the GPU (Metal)
  and in software, HUD, weapon, bots and scoreboard included. It uses
  mipmapped `GL_RGBA8` textures from RGBA bytes, `glTexSubImage2D`, two
  units modulated (the lightmaps), blending (`ONE, ZERO`; `ONE,
  ONE_MINUS_SRC_ALPHA`; `ONE, ONE_MINUS_SRC_COLOR`), the alpha test, and
  a depth range trick (`glDepthRange` flipped each frame). Against the
  RAVE build at the same instruction counts (both paced, they show the
  same intro frames at 38.75 and 41.25 billion): the same pictures, the
  OpenGL build's lightmapped walls a little brighter. That wasn't traced
  further; UT's two renderers light differently, and Blue's drawing of
  what OpenGL asks matches Mesa's in the scenes below. In real time on an
  M5 Mac: on the GPU 60 frames a second (the
  display's refresh) throughout, drawing 13-28% of the time; in software
  41-60 in the intro and about 50 in a match (drawing 85% of the time);
  the RAVE build on the GPU 60, drawing 15%.
- **Checked against Mesa** (`tests/test_opengl.c`, on Linux): four more
  scenes, the earlier six as before. Pixels differing by more than 2 out
  of 6,912:

  | scene | GPU | Mesa | what differs |
  |---|---|---|---|
  | texture (nearest, repeated, replace, decal, modulate, blended) | 3 | 3 | centers on a quad's diagonal (below) |
  | clamp (`GL_CLAMP`, linear, magnified past the edges) | 0 | 3,840 (by up to 191) | Mesa blends with the border color |
  | edge (the same with `GL_CLAMP_TO_EDGE`) | 0 | 0 | |
  | multitexture (two units, linear, in perspective) | 0 | 0 | |

  In "clamp" the pixels whose coordinates are inside the texture are
  checked the same as Mesa's. In "texture", three pixels whose centers are
  exactly on the diagonal two triangles share are drawn by neither in
  `raster.c` (its edge functions are floats, and both triangles' come out
  just negative there); the GPU and Mesa draw them. (Since fixed: exact
  edge functions, see Findings in plan.md; none differ now.)
- **Tests:** `tests/test_glfront.c` (every format and type unpacked, the
  pixel stores, objects and their errors, completeness, the backend's copy
  made, shared and retired, each environment mode for each base format
  against table 3.18, the texture matrix, repeat and clamp, q,
  `glReadPixels`), `tests/test_opengl.c` (the scenes; a swap to a port
  reported for pacing, an offscreen one not), `tests/test_install.c`
  (Apple's libraries found in either folder, the offers, moved to
  Extensions (Disabled) and back, one that can't move).
- Not yet: texture borders, `glCopyTexImage*`, `glDrawPixels`, lighting,
  fog, vertex arrays (milestones 4 and 5); 16-bit drawables dithered;
  lines by OpenGL's diamond-exit rule; and the shared-edge pixels above
  (since fixed).

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
  thread (the Host side section's backend thread is for milestone 7, performance).

Milestone 1:

- **The libraries** (`src/opengl.c`, `src/opengl_table.h`): OpenGLLibrary
  (440 gl and 33 agl functions, and `glc_cc`), OpenGLMemory (25) and
  OpenGLUtility (51), with Apple's fragment names, exports and versions
  (current 1, oldest compatible 0), each export a host-call stub so far
  (recording comes with milestone 2). `tools/gltable.py` made the table
  from Mac OS 9.2.2's libraries (the exports, and the dispatch table's
  offset each gl wrapper calls through, read from its code) and the macOS
  SDK's headers (signatures; AGL's and OpenGLMemory's, which macOS lacks,
  are in the script). They're on the tools volume as "Blue OpenGL
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
  than Mac OS X's vertex array range functions. Blue's contexts are laid
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
  Inc.", renderer "Rage 128 OpenGL Engine", version "1.2 Blue", no
  extensions yet), `glGetError`, `glGet*` for those and a few limits,
  `glFlush`, `glFinish`. OpenGLMemory: `glmMalloc` and the rest through
  `NewPtr`, `NewPtrClear`, `GetPtrSize` and `DisposePtr`; the fills and
  copies on the host; modes and settings accepted. OpenGLUtility: nothing
  yet.
- **The test application** (`src/gltest.c`, "Blue GL Test", on the tools
  volume with `BLUE_TOOLS_GL_TEST=1`): logic in C on the host making
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
  same in two runs, with `BLUE_GPU=1`, and on Linux (Mesa) as on macOS.
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

- **Replace the libraries:** Blue provides OpenGLLibrary (gl and agl),
  OpenGLMemory and OpenGLUtility (glu) under Apple's fragment names, with
  the same exports, and they forward to the host. Blue doesn't load
  Apple's OpenGL engine or its renderers. This works the same on 8.5 and
  8.6, which have no OpenGL, as on 9.x.
- **Record in the guest:** most calls (vertices, colors, state) are stored
  in a buffer in guest memory by a few generated PowerPC instructions, with
  no kernel exit. A host call drains the buffer when it fills, and for any
  call that returns something or reads client memory.
- **Fixed function in Blue, in C:** the host tracks the GL state and does
  transform, lighting, texture coordinate generation and clipping itself,
  then hands screen-space primitives with their fragment state to the
  existing backends, `raster.c` (software, the reference) and `gpu.c`
  (OpenGL 3.3 core), both extended for OpenGL's per-fragment operations.
  No compatibility-profile context on the host.
- **Frames go back into guest memory**, into the drawable (window, memory
  or the whole screen), as the RAVE engine does. Presenting full-screen
  contexts straight in Blue's window comes later, as an optimization.
- **Identity:** an ATI renderer, "Rage 128 OpenGL Engine" with ATI's
  vendor string, as the RAVE engine reports one (see Identity).
- **First target:** Unreal Tournament's OpenGL build, on the UT CD, which
  needs 45 functions.
- **Order:** OpenGL first (the user's choice; this plan proposed Glide
  first, see Order). Milestone 0's groundwork is Glide's too.
- **Installation:** on 9.x Blue's libraries replace Apple's (moved to
  "Extensions (Disabled)" when they're installed); on 8.5 and 8.6 they're
  added.

## What Mac OS 9 ships

### Where it is (verified)

- **Mac OS 9.1 CD (`91.iso`):** the bootable System Folder (what Blue
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
  1999), the Game Sprockets and Blue's 3D engine.
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
- **Apple's software renderer** reports vendor "Apple", renderer "Generic",
  version "1.1" and the extensions `GL_ARB_texture_env_add
  GL_EXT_blend_color GL_EXT_blend_minmax GL_EXT_blend_subtract
  GL_EXT_texture_env_add GL_EXT_texture_lod_bias GL_EXT_abgr GL_EXT_bgra
  GL_SGIS_texture_edge_clamp GL_ARB_multitexture` (9.2.2's
  `gldGetString`, read again: the last one was left out here before). Its
  `gldGetRendererInfo` (9.2.2): accumulation buffers RGB 888, ARGB 8888,
  RGB 16-16-16 and ARGB 16-16-16-16; stencil none or 8 bits; 4 auxiliary
  buffers; window, offscreen, backing store, MP safe, robust, multiscreen
  and compliant, not accelerated or full screen; single and double
  buffers (its color and depth modes come from a table it imports, not
  read). The library maps the info's flags to properties as: window 0x1,
  full screen 0x2, offscreen 0x4, backing store 0x8, MP safe 0x10, robust
  0x40, accelerated 0x100, multiscreen 0x200, compliant 0x400.
- **ATI's renderer info** (9.2.2's `gldGetRendererInfo`): no accumulation
  buffer, no stencil, not offscreen; accelerated, full screen, compliant.
  (Blue's ATI renderer goes beyond it, offering a stencil and offscreen
  drawables, which it draws.)
- **ATI's renderer draws through RAVE**: it imports 12 functions from
  "QuickDraw™ 3D Accelerator" and takes, of a device's RAVE engines, one
  with `kQAOptional_BoundToDevice` (optional-features bit 12) and vendor 1
  (ATI) that passes `QAEngineCheckDevice`. It asks the engine's ID,
  revision, name, texture memory and texture types, and three private ATI
  gestalt selectors (1001, with an argument; 1002, which returns a table of
  functions it calls; 1005, flags). It sets about 50 private ATI
  draw-context tags (1000-1094, mostly through `setInt`, a few through
  `setFloat` and `setPtr`; 1002-1009 carry fog), reads tags 1011 and 1021
  (the hardware generation and ATI's extension table, which Blue's engine
  answers for UT), and uses RAVE's `accessDrawBuffer` and `accessZBuffer`,
  which Blue's engine doesn't support. Its strings: vendor "ATI
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

**(a) Replace the libraries (recommended).** Blue generates OpenGLLibrary,
OpenGLMemory and OpenGLUtility with Apple's fragment names and exports;
their calls go to the host, which does everything.

- For: speed (no OpenGL engine running as emulated PowerPC code; vertices
  transformed by the host); works on 8.5 and 8.6 without Apple's OpenGL,
  so UT's OpenGL build would run on the user's 8.6 install as is; Blue
  controls the identity, extensions and limits; one deterministic software
  path; the same backend as RAVE and Glide.
- Against: Blue implements OpenGL 1.2's fixed-function semantics itself,
  the largest part of the work; Apple's own OpenGL is set aside on 9.x.

**(b) A renderer under Apple's engine.** A `shlb` of creator `gld1` that
Apple's engine loads like ATI's, exporting the 22 `gld` functions.

- For: Apple's engine keeps all the API and state semantics.
- Against: the interface is private and would have to be learned from the
  engine and two renderers (vertex buffer layouts, the render dispatch
  table, textures, drawables); transform and lighting stay in emulated
  PowerPC code; 9.x only (or wherever Apple's OpenGL is installed).

**(c) Apple's OpenGL over Blue's RAVE engine.** Apple's ATI renderer
already turns OpenGL into RAVE calls; Blue's engine would have to pass for
an ATI engine to it.

- For: no new library; Blue's RAVE path is deterministic already.
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

- The libraries join the guest tools volume as one folder ("Blue OpenGL":
  three files with Blue's own creator and file names, fragment names
  OpenGLLibrary, OpenGLMemory and OpenGLUtility, `cfrg` versions as
  Apple's: current 1, oldest compatible 0, so programs linked against
  Apple's, including Apple's OpenGLUtility, accept them). The install
  prompt (another branch) puts them in Extensions.
- On 9.x, Apple's same-named libraries are in Extensions too. Rather than
  rely on which of two same-named fragments the Code Fragment Manager
  picks (an open question to test), the install prompt moves Apple's
  OpenGLLibrary, OpenGLMemory and OpenGLUtility to "Extensions
  (Disabled)", saying so, and moves them back on uninstall (as built:
  Blue has no uninstall, so once Blue's are deleted the offer is to put
  Apple's back; see Status). Apple's engine
  and renderers are then never loaded (only Apple's OpenGLLibrary loads
  the engine).
- As with the 3D engine, the library's data carries its version, so one
  installed by an older version of Blue is noticed and the user told to
  install it again.
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

**(ii) Fixed function in Blue, on the core path (recommended).**

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
oracle: the tests draw the same command streams through Blue's front end
and through Mesa, and compare (`(i)` as a test, not as the product).

What's not planned: the imaging subset's convolution, histogram and
minmax (they get `GL_INVALID_OPERATION`, and `GL_ARB_imaging` isn't
advertised). (The accumulation buffer wasn't planned either; milestone 8
did it, on the host, as the generic renderer's.) (Evaluators weren't planned at first,
and selection and feedback were to be simple; milestone 6 did both, from
the front end's output.)

**Extensions to offer:** the engine's list above (all of it is within
reach), plus the secondary color extension whose functions are exported
anyway, and `GL_EXT_texture_filter_anisotropic`. (S3TC and paletted
textures were proposed too, but Apple's 9.2.2 offers neither for a Rage
128: see milestone 6.) Limits: 2 or 4 texture units, 8
lights, 6 clip planes, 8 stencil bits, 16/24-bit depth, textures to 1024
or 2048 (see Identity).

### AGL and presentation

- **Pixel formats and renderer info:** one renderer, accelerated, ID
  0x00021000 (as ATI's on 9.1), window, full-screen and offscreen
  drawables, RGBA in 16 and 32 bits, depth 16/24/32, 8-bit stencil, no
  accumulation buffer, double or single buffering. (As built, since
  milestone 8: a second, the generic renderer, for pixel formats with an
  accumulation buffer.) `aglChoosePixelFormat`
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
  switches the display mode through the Display Manager (a callout of the
  library's, as the Blue Enhancements helper switches modes) and draws to
  the whole screen; destroying the context, setting another drawable or
  the process quitting switches back. (As built: see milestone 6. Apple's
  9.2.2 hands the size to its engine and renderer, which import no
  Display Manager call; Blue does what the AGL reference says.) Most
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
  the whole screen (full screen, or DrawSprocket's front buffer), Blue's
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
  `glBitmap` (as built: see milestone 6).

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
Blue's version (Apple's ATI renderer said `1.1.ATI-%d.%d`; programs parse
the leading number); `GL_MAX_TEXTURE_SIZE` 1024 and 2 texture units, as a
Rage 128 had (from memory); and the extension list above, which is more
than a Rage 128 had.
The alternative is "ATI Radeon OpenGL Engine" (2048 textures, more units),
for later games that expect a Radeon; or honest Blue strings, which
games of the time don't know. **The user decides.** Whatever is chosen,
`GL_EXTENSIONS` describes what Blue really does. (As built: ATI's
strings; and since milestone 8, for pixel formats with an accumulation
buffer, Apple's generic renderer's vendor "Apple" and renderer "Generic",
with Blue's version and extensions.)

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
Blue's own: a test application generated like Blue Enhancements, whose
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
   3D textures, offscreen drawables, GLU on the host.
6. **Completeness:** `aglSetFullScreen`, `aglUseFont`, selection and
   feedback, evaluators; S3TC and paletted textures only if Apple's 9.2.2
   listed them for the Rage 128 (it doesn't).
7. **Performance:** direct presentation for full-screen contexts; reading
   back only what changed; transform in a vertex shader; bigger batches.
   The same checks, with Quake III's demo and UT as the game tests. (As
   done: measured first, then the costs measured fixed; see Status.)
8. **The rest of OpenGL 1.2:** logic ops, line and polygon stipples,
   textured lines and points, antialiased ones, lines by the diamond-exit
   rule, dithering in 16-bit drawables, the accumulation buffer (as Apple's
   9.2.2 renderers offered it). The same checks; RAVE's pixels and the
   games' speed unchanged.

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
  and AArch64 (CI); and the same with `BLUE_GPU=1` within tolerance.
- **Determinism:** a scripted UT OpenGL session in instruction time twice,
  identical screenshots and instruction counts.
- **By hand:** UT's OpenGL build in a window and full screen, resolution
  changes, Restart in the middle, two OpenGL programs at once,
  `BLUE_GPU_FAIL` falling back to software.

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
  implementation; Blue's libraries keep textures on the host, so the
  guest needs less, but games size their partitions by their own rules.

## Open questions

- Identity: Rage 128, Radeon, or Blue's own strings? (User.)
- Which of two same-named fragments in Extensions CFM picks (not tried).
  Disabling Apple's is enough on 9.2.2 (verified: UT's OpenGL build then
  links to Blue's, even in the session that moved Apple's); 9.1 isn't
  tried (installing 9.1 onto a disk image isn't done yet in Blue).
- Did a Mac OS 9 `aglMacro.h` exist, and does any target call through
  `glc_cc` or the context's table? (Apple's OpenGLUtility reads `glc_cc`:
  it imports it.)
- Which games use `aglSetFullScreen` rather than DrawSprocket, and what
  Apple's did with the menu bar and other applications' windows (and how
  its renderers switched modes, if they did: none of its libraries
  imports the Display Manager).
- OpenGLUtility's tessellators and NURBS renderers keep their objects and
  properties but draw nothing: left until a program needs them.
- In which order Apple's engine listed its renderers to
  `aglQueryRendererInfo` and `aglNextRendererInfo` (Blue: ATI's, then the
  generic one), and which a pixel format both could draw went to without
  `AGL_ACCELERATED` (Blue: ATI's, unless it asks for an accumulation
  buffer).
- The Rage 128's dither (its matrix, and whether Apple's ATI renderer
  turned it off for `glDisable(GL_DITHER)`): not found; Blue's is a 4 x 4
  ordered dither.
- Which folder Apple's engine searches for renderers (the `FindFolder`
  argument wasn't checked): only matters for option (b).
- Whether to offer Apple's own OpenGL on 9.x later through the RAVE engine
  (option (c)), for programs Blue's libraries get wrong.

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
