# Plan: 3dfx Glide for Mac OS games

Some Mac games of 1997-2001 draw with 3dfx's Glide, the Voodoo cards' own
API, through a shared library Mac Voodoo drivers installed. Blue has no
Voodoo, so this plan gives it a Glide library of its own, generated like
the other guest tools, which hands each call to the host; there a model of
the Voodoo's rendering state drives the 3D backend the RAVE engine uses
(GPU in real time, software otherwise, so instruction-time runs stay
reproducible), and the Glide image is shown in Blue's window in place of
the Mac's screen, as a Voodoo's own video output replaced it.

This is a plan of its own rather than part of docs/opengl-plan.md because
it's a different API with its own milestones and games, small enough to
build and test alone, and it's recommended first (see Order in
opengl-plan.md). Both use the same backend extensions, presentation and
pacing; those are described in opengl-plan.md (Host side, AGL and
presentation) and referred to here.

## Status

Research and design only. The groundwork this plan shares with OpenGL is
built (opengl-plan.md, milestone 0, which the user chose to do first): the
fragment builder's exports and fragment names, host calls with floats, and
the backend's two texture units combined in stages, blend equations,
constant colors, color masks and fog coordinate; what Glide adds there
(its combine units' general form, fog tables, chroma key, W buffering) is
still to do. Facts checked on the media are marked **verified**; the rest
comes from the documents and projects named, or from memory, and is marked
so.

## Decisions (proposed)

- **A library named as 3dfx's:** a `shlb` whose fragment is
  `3DfxGlideLib2.x` (Glide 2.x), exporting Glide 2's functions; Glide 3
  (`3DfxGlideLib3.x`) later, for the games that need it.
- **One host call per Glide call.** Glide draws a triangle or a polygon
  list per call, passing vertex pointers; an exit each (about 50 ns) is
  cheap enough. The OpenGL plan's guest-side recording can be added later
  if a game needs it.
- **A register-level Voodoo model on the host:** Glide calls set the
  Voodoo's rendering registers, as 3dfx's own (open-sourced) Glide does;
  `raster.c` and a `gpu.c` shader interpret those registers. Vertices are
  already in screen space, so OpenGL's front end isn't involved.
- **Its own display:** while a Glide window is open, Blue's window shows
  the Glide image (scaled as the Mac's screen is), and the Mac's screen
  stays as it was in guest memory underneath; closing Glide, or the game
  quitting or crashing, brings the Mac's screen back.
- **Identity:** a Voodoo2 with two texture units (see Identity; the user
  decides).

## Glide on Mac OS

### Verified on the media

- **Unreal Tournament 4.32** (the install on `hd86.img`): one application
  binary with the Glide and RAVE renderers built in (`UGlideRenderDevice`,
  `URaveRenderDevice`; source file `UnGlide.cpp`). It imports 54 functions,
  all **weak**, from a library whose fragment name is **`3DfxGlideLib2.x`**:
  - `grAlphaBlendFunction`, `grAlphaCombine`, `grAlphaTestFunction`,
    `grAlphaTestReferenceValue`, `grBufferClear`, `grBufferNumPending`,
    `grBufferSwap`, `grChromakeyMode`, `grChromakeyValue`,
    `grColorCombine`, `grColorMask`, `grConstantColorValue`,
    `grDepthBiasLevel`, `grDepthBufferFunction`, `grDepthBufferMode`,
    `grDepthMask`, `grDitherMode`, `grDrawPlanarPolygonVertexList`,
    `grDrawPolygonVertexList`, `grErrorSetCallback`, `grFogColorValue`,
    `grFogMode`, `grFogTable`, `grGammaCorrectionValue`,
    `grGlideGetVersion`, `grGlideInit`, `grGlideShutdown`, `grHints`,
    `grLfbLock`, `grLfbReadRegion`, `grLfbUnlock`, `grSstQueryBoards`,
    `grSstQueryHardware`, `grSstScreenHeight`, `grSstScreenWidth`,
    `grSstSelect`, `grSstWinClose`, `grSstWinOpen`,
    `grTexCalcMemRequired`, `grTexClampMode`, `grTexCombine`,
    `grTexDownloadMipMapLevel`, `grTexDownloadMipMapLevelPartial`,
    `grTexDownloadTable`, `grTexFilterMode`, `grTexLodBiasValue`,
    `grTexMaxAddress`, `grTexMinAddress`, `grTexMipMapMode`,
    `grTexSource` (50)
  - `guAlphaSource`, `guColorCombineFunction`, `guFogTableIndexToW`,
    `guTexCombineFunction` (4)

  No `grDrawTriangle`: it draws everything as polygon vertex lists. Its
  vertices are **60 bytes** apart (it multiplies vertex indices by 60 in
  its drawing functions): `GrVertex` with 9 floats and two texture units'
  (s/w, t/w, 1/w), so the SDK it was built with had `GLIDE_NUM_TMU` 2.
- Its strings show what it asks and logs: "Initializing Glide...", "Glide
  library not found!", "Found Glide: %s" (the version string), "Glide
  info: Type=%i, fbRam=%i fbiRev=%i nTexelfx=%i Sli=%i", "Glide tmu %i:
  tmuRev=%i tmuRam=%i Space=%i", "Glide: Resolution %i failed, falling
  back...", "Glide: Refresh %i failed...", "Glide: Color buffers %i
  failed...", "Flushed Glide TMU %i", "Unreal 3dfx Pict %04d" (its
  screenshots, through `grLfbReadRegion`), and functions `Is3DfxAvail`,
  `Is3DfxViewport`, `Passthrough3DfxOff`, `Mac3DfxCleanup`.
  `GlideDrv.int` calls the renderer "3dfx Glide for Macintosh" and asks
  "Do you have a 3dfx card installed?".
- The OpenGL build on the UT CD imports the same 54.
- **No Glide library** on the 8.6 install, the 9.1 CD (its installer's
  archive included) or the UT CD; the only Glide file on them is UT's
  `GlideDrv.int` (on the 8.6 install and the UT CD).

### From documents and third parties

- **Fragment and file names:** a 2000 article on g-news.ch (on running a
  Voodoo3 with a Voodoo1) says the Voodoo1's Glide driver file is called
  "3DfxGlideLib2.x" and the Voodoo3's "3Dfx GlideLib2.x", that the two
  differ, and that a game finds a Glide library placed in its own folder
  (from memory, the Code Fragment Manager looks in the application's
  folder before Extensions).
  MacGLide installs its library as `3DfxGlideLib2.x` in Extensions, and
  says Diablo II and Waterrace need `3DfxGlideLib3.x` (Glide 3).
- **Mac drivers** (soggi.org's driver archive): Voodoo2 for Mac OS 8.x
  1.0b5 beta (January 2000, with 3dfx's OpenGL 1.0b11), Voodoo3 for Mac OS
  8.1-9.x betas 12 and 13 (2000), VoodooMAC 4500/5500 1.1.2 (November
  2000) and 1.1.3b. Cards: the Techworks Power3D (Voodoo1),
  Microconversions' Voodoo2 cards (the iMac "Game Wizard" among them),
  3dfx's Voodoo3 2000/3000 for Mac, the VoodooMAC 4500 and 5500.
- **Glide 2 and 3:** per a former 3dfx Mac engineer quoted on
  xlr8yourmac, the Voodoo1 and 2 drivers were built on Glide 2, full
  screen only; from the Voodoo3 on, Glide 3, with windows and 32-bit color,
  under 3dfx's RAVE and OpenGL drivers too. 3dfx's open-sourced Glide 3
  tree has a Mac build header (`macglide3.h`, Metrowerks settings,
  `GLIDE_DEFAULT_GAMMA` 1.3).
- **MacGLide** (Jens-Olaf Hemprich, LGPL, 2003-2008; a Mac OS 9 and
  Classic port of OpenGLide): a Glide 2 library for Mac OS 9 over Apple's
  OpenGL. It answers `grSstQueryHardware` as a configurable board (Voodoo
  by default; fbiRev 2, tmuRev 1, 16 MB texture and 8 MB frame buffer
  memory), `grGlideGetVersion` "Glide 2.45 - ...", and keeps a
  compatibility list (below). It's prior art for everything here,
  including its framebuffer emulation for LFB access.

### How games detect a Voodoo

From UT's strings and imports (verified) and Glide 2's API (3dfx's SDK
headers, as MacGLide carries them):

1. The weak import resolved: `grGlideInit` isn't
   `kUnresolvedCFragSymbolAddress` (else "Glide library not found!").
2. `grGlideGetVersion(char[80])`: a version string.
3. `grGlideInit()`, then `grSstQueryBoards(&hw)` (`num_sst` > 0) and
   `grSstQueryHardware(&hw)`: per board, its type (`GR_SSTTYPE_VOODOO` 0,
   `SST96` 1, `AT3D` 2, `Voodoo2` 3) and its configuration (`fbRam` in MB,
   `fbiRev`, `nTexelfx` = texture units, `sliDetect`, and per unit
   `tmuRev` and `tmuRam`).
4. `grSstSelect(0)`, `grSstWinOpen(window, resolution, refresh, color
   format, origin, color buffers, aux buffers)`, falling back to other
   resolutions, refresh rates and buffer counts if it fails (as UT does).
5. `grTexMinAddress` and `grTexMaxAddress` per unit for texture memory
   ("Space" in UT's log).

## The API surface

Glide 2.x has roughly 100 `gr` and 25 `gu` functions (counted from the SDK
headers MacGLide carries, which mix in some of Glide 3's). By group, with
UT's needs in bold:

- **Setup:** **`grGlideInit`**, **`grGlideShutdown`**,
  **`grGlideGetVersion`**, **`grSstQueryBoards`**, **`grSstQueryHardware`**,
  **`grSstSelect`**, **`grSstWinOpen`**, **`grSstWinClose`**,
  **`grSstScreenWidth`**/**`Height`**, `grSstOrigin`, `grSstControl`,
  `grSstIdle`, `grSstIsBusy`, `grSstVRetraceOn`, `grSstStatus`,
  `grSstPerfStats`, **`grHints`**, **`grErrorSetCallback`**,
  `grGlideGetState`/`SetState`.
- **Buffers:** **`grBufferClear`**, **`grBufferSwap`**,
  **`grBufferNumPending`**, `grRenderBuffer`, `grClipWindow`,
  **`grColorMask`**, **`grDepthMask`**, **`grDitherMode`**.
- **Drawing:** `grDrawTriangle`, `grDrawLine`, `grDrawPoint`,
  `grDrawPolygon`, **`grDrawPolygonVertexList`**, `grDrawPlanarPolygon`,
  **`grDrawPlanarPolygonVertexList`**, the `grAADraw*` forms, `grCullMode`;
  `guDrawTriangleWithClip`, `guAADrawTriangleWithClip`,
  `guDrawPolygonVertexListWithClip`.
- **Pixel pipeline:** **`grColorCombine`**, **`grAlphaCombine`**,
  **`grConstantColorValue`**, `grAlphaControlsITRGBLighting`,
  **`grAlphaBlendFunction`**, **`grAlphaTestFunction`**,
  **`grAlphaTestReferenceValue`**, **`grChromakeyMode`**,
  **`grChromakeyValue`**, **`grDepthBufferMode`**,
  **`grDepthBufferFunction`**, **`grDepthBiasLevel`**;
  **`guColorCombineFunction`**, **`guAlphaSource`**.
- **Fog:** **`grFogMode`**, **`grFogColorValue`**, **`grFogTable`**;
  **`guFogTableIndexToW`**, `guFogGenerateExp`, `guFogGenerateExp2`,
  `guFogGenerateLinear`.
- **Textures:** **`grTexMinAddress`**, **`grTexMaxAddress`**,
  **`grTexCalcMemRequired`**, `grTexTextureMemRequired`,
  `grTexDownloadMipMap`, **`grTexDownloadMipMapLevel`**,
  **`grTexDownloadMipMapLevelPartial`**, **`grTexDownloadTable`**,
  `grTexDownloadTablePartial`, **`grTexSource`**, `grTexMultibase`,
  `grTexMultibaseAddress`, **`grTexCombine`**, **`grTexFilterMode`**,
  **`grTexClampMode`**, **`grTexMipMapMode`**, **`grTexLodBiasValue`**,
  `grTexDetailControl`, `grTexNCCTable`; **`guTexCombineFunction`** and
  the `gu` texture manager (`guTexAllocateMemory`, `guTexDownloadMipMap`,
  `guTexSource`, `guTexMemReset`, `guTexMemQueryAvail`, ...), `gu3dfLoad`
  and `gu3dfGetInfo` (texture files).
- **Linear frame buffer:** **`grLfbLock`**, **`grLfbUnlock`**,
  **`grLfbReadRegion`**, `grLfbWriteRegion`, `grLfbConstantAlpha`,
  `grLfbConstantDepth`, `grLfbWriteColorFormat`, `grLfbWriteColorSwizzle`.
- **Gamma:** **`grGammaCorrectionValue`**.

Structures the host reads from or writes to guest memory, big-endian:
`GrVertex` (60 bytes as UT has it), `GrTexInfo` (small and large LOD,
aspect ratio, format, data pointer), `GrHwConfiguration` (up to 4 boards;
its per-unit array is also sized by `GLIDE_NUM_TMU`, 2 by default in the
SDK's `glidesys.h`, as in UT), `GrLfbInfo_t` (size, pointer, stride,
write mode, origin), fog tables (64 bytes), palettes and NCC tables.

## Design

### The library

- Generated by `src/tools.c` like the 3D engine: a `shlb` with fragment
  `3DfxGlideLib2.x`, file name "Blue Glide" (Blue's creator), on the
  tools volume, installed into Extensions by the install prompt. It needs
  what the OpenGL plan's milestone 0 adds to the PEF builder (more than 64
  exports, `cfrg` names) but not per-export code: every entry is the usual
  host-call stub.
- Floats: `grGammaCorrectionValue`, `grTexLodBiasValue` and the
  `guFogGenerate*` functions take one float, `guFogTableIndexToW` returns
  one; f1 in and out, which host calls already carry. Everything else is
  integers and pointers.
- Vertices are read from guest memory at the call; the vertex size comes
  from the Glide version the library answers as (Glide 2's 60-byte layout,
  checked against each game: a game built with three units' worth would
  use 72).
- `grErrorSetCallback`'s function is kept; Glide calls it only on fatal
  errors, which Blue reports in its own output instead.
- The library's CFM termination routine (run when the application quits,
  also through `ExitToShell` after a crash) closes the Glide window if the
  game didn't. A crashed Voodoo game used to leave the screen to the
  Voodoo; GlideHack, a Mac Glide add-on, advertised "crash cleanup" for
  that.

### The Voodoo model on the host (`src/glide.c`)

- **State as the hardware's registers:** `fbzColorPath`, `fbzMode`,
  `alphaMode`, `fogMode`, `lfbMode`, `clipLeftRight`/`clipLowYHighY`,
  `color0`, `color1`, `chromaKey`, `fogColor`, `zaColor`, the 64-entry fog
  table, and per texture unit `textureMode`, `tLOD`, `tDetail`, the base
  addresses. Each Glide call sets bits as 3dfx's Glide 2 source does
  (released in 1999 under 3dfx's Glide license, so readable for the
  mapping); `raster.c` and the shader interpret the bits as the hardware
  did (MAME's Voodoo emulation is a public reference for that). One
  interpreter then serves Glide 2 and 3, and the state per batch is a few
  dozen words.
- **The combine units:** per texture unit (unit 1's output is unit 0's
  "other" input), then color and alpha: the hardware's general form, zero
  or the other input, minus the local one or not, times a factor chosen
  from about ten sources, plus the local color or alpha or nothing,
  inverted or not. Glide's `grColorCombine` functions and factors, and the
  `gu` shorthands, all come down to that.
- **Texture memory:** each texture unit has a byte array on the host (its
  `grTexMaxAddress`), into which downloads copy texels as the game lays
  them out (big-endian 16-bit texels byte-swapped on the way in).
  `grTexSource` names a texture by unit, address, format, LOD range,
  aspect ratio and odd/even; the host decodes it (formats 8-bit RGB 332,
  YIQ 422 with its NCC table, alpha, intensity, alpha-intensity 44, P8
  with the palette, ARGB 8332, AYIQ 8422, RGB 565, ARGB 1555, ARGB 4444,
  alpha-intensity 88, AP 88) to RGBA8 for the backends, and caches it by
  those keys and the table's version. A download overlapping a cached
  texture's bytes drops it, as do new palettes or NCC tables for textures
  that use them.
- **Texture coordinates and filtering:** s/w and t/w run 0-256 across the
  texture's longer side (the aspect ratio scales the other), 1/w comes per
  unit; point or bilinear filtering, clamp or wrap, mipmaps by
  `grTexMipMapMode` (nearest, dithered or trilinear with two units), LOD
  bias. Levels are picked per pixel, as RAVE's are on the GPU.
- **Depth:** Z buffering (1/z in 16 bits, from `ooz`) or W buffering
  (1/w in the Voodoo's 16-bit floating-point form), compare functions,
  depth bias, depth mask, the aux buffer as alpha instead.
- **The rest of the pipeline:** fog from the table (indexed by w),
  iterated alpha or z, with the add and multiply modes; alpha test;
  chroma key (against the color the hardware compares); blending with
  Glide's factors (including the pre-fog color and alpha saturate); color
  masks; clip window; culling. Dithering is ignored: the image is drawn in
  32 bits (a 16-bit look with the Voodoo's dither could be an option).
- **Buffers:** front and back color buffers and the aux buffer, at the
  resolution `grSstWinOpen` asked for (Glide 2's list runs from 320x200 to
  1600x1200); `grBufferClear` (color, alpha, depth), `grBufferSwap`
  (with its interval: see pacing in opengl-plan.md),
  `grRenderBuffer` to draw to the front buffer.

### Linear frame buffer access

The Voodoo's frame buffer was memory the processor could read and write;
games use it for screenshots (UT: `grLfbReadRegion`), movies, 2D overlays
and sometimes per-pixel effects.

- `grLfbLock` returns a pointer and a stride into guest memory: a buffer
  the library gets at `grSstWinOpen` (through a `NewPtrSys` callout, the
  size of one buffer at the window's resolution in the largest write
  format), or, alternatively, a range of guest physical memory Blue maps
  as the board's (as the display's frame buffer is mapped), which needs
  no Mac OS allocation.
- A **read** lock waits for the backend, reads the locked buffer back and
  converts it into the guest buffer in the lock's format (565, 555, 1555,
  888, 8888, depth).
- A **write** lock does the same, so pixels the game doesn't write keep
  their values, and at `grLfbUnlock` the host converts back the rows the
  game wrote and puts them into the buffer as a bitmap drawn over it.
  Which rows: those in pages written since the lock, from the guest page
  protections Blue already manages for the JIT (write-protect the buffer
  at lock, note faults), or by comparing with the converted copy.
- With `pixelPipeline` set, written pixels go through fog, blending,
  depth and so on: they're drawn as points with the current state (rare).
- `grLfbReadRegion` and `grLfbWriteRegion` are copies through a host
  call.

Reading back for every write lock costs a GPU sync; games that lock every
frame (movies) are where a cheaper path (keeping the last frame's copy
when nothing else drew) would pay.

### Presentation

- At `grSstWinOpen`, Blue's window switches from the Mac's screen to the
  Glide image, scaled to the window (as the Mac's screen is, keeping its
  shape); at `grSstWinClose`, `grGlideShutdown` or termination, back.
  Like a Voodoo with its pass-through cable, the Mac's screen in guest
  memory isn't touched meanwhile.
- In real time the image is the GPU's, read back into host memory at each
  swap and handed to `display.c` (`gpu.c` never shares the window's
  context); in software it's `raster.c`'s target. Either way, not guest
  memory, so the guest's execution never depends on it unless it reads
  the frame buffer, which goes through the deterministic path below.
- Screenshots (`--screenshot`, the F12 menu, input scripts) take the
  Glide image while it's shown.
- **Gamma:** `grGammaCorrectionValue` applies at presentation, from the
  value the game sets (3dfx's Mac Glide 3 defaulted to 1.3; the Voodoo2
  Mac drivers' default looked dark, per a reader's note on xlr8yourmac).
- **The mouse:** games read the Mac's cursor in Mac screen coordinates,
  usually after switching the Mac's screen (DrawSprocket) to the Glide
  resolution. If they differ, Blue maps the host pointer to the Glide
  image's scale; to check per game (MacGLide's notes mention Carmageddon's
  pointer needing matching resolutions).
- **Reproducibility:** instruction-time runs draw in software, as RAVE
  does, so screenshots and LFB reads are the same on every host.

### Identity

Proposed: one board, `GR_SSTTYPE_Voodoo2`, `fbiRev` 2, `fbRam` 4, two
texture units (`nTexelfx` 2), `sliDetect` false, each unit `tmuRev` 1 and
`tmuRam` 4, texture addresses 0 to 4 MB per unit; version string "Glide
2.46" or so (UT prints it; nothing seen parses it).

Why a Voodoo2: two texture units let UT and Unreal draw lightmaps and
detail textures in one pass, and the Mac had Voodoo2 cards. Against:
games may cap a Voodoo2's resolutions at 800x600 (1024x768 with SLI);
reporting SLI, or more memory, would lift that if a game lets it. MacGLide
answered as a Voodoo by default. **The user decides**; the answers are a
few constants.

### Glide 3 (later)

`3DfxGlideLib3.x` for Diablo II and Waterrace (per MacGLide) and whatever
else wants it: Glide 3 replaces the vertex structure with
`grVertexLayout`, adds `grDrawVertexArray`, `grGet`, `grGetString` and
`grCoordinateSpace`, and windowed rendering. The register model is the
same; the API layer is new (about 100 functions).

## Targets

Verified: **Unreal Tournament 4.32**, installed on the 8.6 disk, with its
Glide renderer built in.

MacGLide's compatibility list (its `Compatibility.txt`, games that worked
with its Glide 2 library): Carmageddon, Carmageddon Splat Pack,
Carmageddon 2, Descent 1 and 2, Descent 3, Deus Ex, Star Trek: Deep Space
Nine - The Fallen, Falcon 4.0, F/A-18 Korea, Future Cop: L.A.P.D., Myth:
The Fallen Lords, Quake (3Dfx), Rune, Tomb Raider (Gold), Tomb Raider II
(Gold), Tomb Raider III, Unreal. Needing Glide 3: Diablo II, Waterrace.
xlr8yourmac adds Myth II (through Microconversions' Glide driver).

Several of these have no other accelerated renderer on the Mac (from
memory: Tomb Raider I-III, Carmageddon, Falcon 4.0), which is Glide's
value beyond UT.

## Shared with OpenGL

Built once, by whichever comes first (opengl-plan.md milestone 0):

- The PEF builder's growth (exports, `cfrg` names; per-export code is for
  OpenGL's recording only).
- The backend's richer state in `raster.c` and `gpu.c`: two or more
  texture units with a combining stage, fog by table or factor, chroma
  key, alpha test, W buffering, more blend factors, depth bias. Glide's
  register interpreter and OpenGL's texture environments are two ways of
  filling the same per-unit combine state.
- Presentation (Glide's direct display is the "presenting directly" path
  OpenGL's full-screen contexts use later), pacing to the guest's
  refresh, cleanup at termination, gamma.
- Tests: the software/GPU comparison harness of `test_gpu.c`.

## Milestones

Each leaves the tree working, `make test` passing on Linux and macOS.

1. **The library and detection.** `3DfxGlideLib2.x` generated and
   installable; the setup functions answer as a Voodoo2; `grSstWinOpen`
   shows a cleared image in the window, `grBufferClear` and
   `grBufferSwap` work, `grSstWinClose` and termination give the Mac's
   screen back. Done when UT's first-time setup offers "3dfx Glide for
   Macintosh" and its log shows the board, and quitting UT (or killing it
   in a script) restores the screen.
2. **Untextured drawing.** Polygon lists, triangles, lines, points; color
   and alpha combine with iterated and constant colors; Z and W
   buffering; blending, alpha test, clip window, culling. Done when known
   scenes match between `raster.c` and `gpu.c`, and the software output
   matches hand-computed pixels for each combine function.
3. **Textures, then UT.** Texture memory, downloads (whole, per level,
   partial), `grTexSource`, every format with palettes and NCC tables,
   filtering, clamping, mipmapping, LOD bias, two units combined. Done
   when UT plays with its Glide renderer on the GPU and in software, and
   looks like its RAVE renderer.
4. **The rest of Glide 2.** Fog tables, chroma key, the linear frame
   buffer (UT's screenshots), gamma, the `gu` texture manager and fog
   helpers, `guDrawTriangleWithClip`, `.3df` files (through File Manager
   callouts). Done when the other games on the list the user has run (Tomb
   Raider, Carmageddon, Myth and so on).
5. **Glide 3**, if wanted.

## Tests

- **`tests/test_glide.c`**, the host side driven with call sequences:
  - exact: each Glide call's register bits against tables taken from 3dfx's
    Glide 2 source; the combine units for every function and factor
    against a reference written in the test; every texture format's
    decoding (NCC and palettes included) against hand-computed texels; the
    texture cache dropping textures that downloads overwrite; W-buffer
    encoding; fog table lookup; LFB round trips in each format.
  - software against GPU, as `test_gpu.c` does for RAVE (edges only).
- **Determinism:** a scripted UT session with the Glide renderer in
  instruction time, run twice and on x86_64 and AArch64: identical
  screenshots.
- **By hand:** UT in a window and full screen, resolution changes,
  crashing out of a game (the screen comes back), `BLUE_GPU_FAIL`.

## Risks

- **Exact Voodoo look:** 16-bit dithered output, the Voodoo's LOD
  selection and its bilinear filter's precision differ from what Blue
  draws; accepted (32-bit output looks better), with the 16-bit look as a
  possible option.
- **LFB-heavy games** (movies written every frame) cost a readback per
  lock; the cheaper path above may be needed.
- **Detection outside Glide:** a game that also looks for the card itself
  (a driver in the device tree, a 3dfx control panel, Gestalt) won't find
  it; none is known.
- **Games needing Glide 3**, and games that need a different board type or
  more texture memory: constants to adjust, per game if need be.
- **Mouse coordinates** when the Glide and Mac resolutions differ.

## Open questions

- Board identity: Voodoo2 (two units) or Voodoo, how much memory, SLI?
  (User.)
- The Mac Glide 2 SDK's `GLIDE_NUM_TMU` and vertex size for each game
  (UT: 60 bytes, verified); whether any Mac game used three units' worth.
- Whether to tie the Glide image's resolution to the Mac's (switching the
  Mac's screen at `grSstWinOpen`) for games that read the mouse.
- LFB memory: a `NewPtrSys` buffer, or a board range in guest physical
  memory?

## Size

Rough, with tests: the library table and stubs 500 lines; the register
model and calls 1,500; texture memory, decoding and cache 800; LFB 400;
presentation, gamma and cleanup 300; backend growth (shared with OpenGL)
about 1,000; tests 1,000. About 5,500 lines, of which milestone 3 (UT) is
about 4,000.
