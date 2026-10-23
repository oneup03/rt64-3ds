# Plan: the vertex transform on the GPU

Status (October 23, 2026): phase 1 built and correct (pixel-identical to
the CPU path in dl_test, the demos right in Azahar), and **measured as no
gain**: on the Old 3DS the A/B showed the same 17-21 fps in the rap and
13-19 in the DK TV maps either way with core 0 at 91-97 %, and Azahar's
renderer profile shows why: the read's vertex and triangle stages cost
the same with 95 % of the rap's triangles on the GPU (vertices 4.0 vs 4.4
ms, triangles 9.4 vs 8.6) while the replay's draws cost three times more
(3.5 vs 1.2 ms). The per-vertex transform and lighting this moved were a
fifth of the read at most; the rest is the interpreter's per-command,
per-load and per-triangle bookkeeping, which the path still does and adds
to (program switches, palette uploads, the band and palette tests). Since
then the two programs became one (`rt64_3ds.v.pica`, a `pre` bool telling
a CPU-path draw from a GPU-path one, one 32-byte vertex layout for both,
the raw vertices in the same buffer as the transformed ones) and the GPU
path's uniforms go out only when a draw's differ from the last: no program,
layout or buffer switch per draw. Azahar's renderer profile of the rap
with that (the same demo, both rows): the read's vertex stage 3.9 ms
against the CPU path's 4.1 and its triangle stage 8.5 against 7.9, the
replay's draws 3.6 ms against 1.3. The read cannot shrink much: the
guard-band test transforms every position anyway (the clip stays, see
below), so only the lighting left the CPU; and the replay's draws cost
the palette uploads, since DK64 loads a matrix every 15 triangles or so
and each slot is 40 words of uniforms per draw, which is about what
transforming its dozen vertices cost. The path cannot pay off for a game
with that matrix churn; it might for one that loads a few matrices per
hundreds of triangles. The
Transform row stays, off by default on both consoles, as an experiment;
the work goes to the interpreter's own overhead, measured on the console
with tools/dl_bench_3ds's stage cuts (DL_BENCH_SKIP_VTX / SKIP_TRI /
NO_EMIT / NO_PROBE), which is the phase 0 this plan skipped. Lessons from
the shader debugging are in the port skill's pitfalls. Phase 0 was skipped at the owner's call (the logs
covered the margin). The measurements are DK64's on an Old 3DS, the game
on core 1 and the renderer on core 0 (the Auto layout).

## Why

With the game's own costs down (the bone builder native, the matrix
helpers native, preemption), an Old 3DS running DK64 is limited by the
renderer's CPU work on core 0: 17-21 fps with core 0 at 92-96 % while the
game's core has room. The read profile of a heavy frame (the DK Rap, about
140 draws and 4,000 vertices, `dbg_profile=1`, whose clock reads inflate
the figures somewhat):

| stage of the list's read | ms a frame |
|---|---|
| triangles: cull, screen cull, guard band, texel coordinates, the three vertices written out | 20-23 |
| vertices: transform, lighting, texel scale | 9-10 |
| matrices, texture loads, the rest | about 7 |
| **the read** | **38-41** |
| the replay into citro3d (textures, TEV, state, draws) | 7-10 |
| the GPU, both eyes (runs beside the CPU) | 14-16 |

The GPU is at under half of a 33 ms frame. The CPU does what the N64's RSP
did: every vertex transformed and lit, every triangle culled, tested and
written to the vertex buffer as 84 bytes (three `Vtx3ds`). `perf` on the
host over the same capture agrees: the triangle path, the vertex path and
the command loop each take a quarter to a third, spread over many lines,
nothing to fix in one place. The World distance setting proved the point
the other way: dropping a fifth of the triangles after their vertices were
transformed moved no frame rate.

The gain to expect, by the figures above: the vertex path to a copy (16
bytes a vertex, about 1 ms), the triangle path to three indices a triangle
plus a cheap guard-band check (about 4 ms), the rest as it is: a heavy
frame's read from about 40 ms to 12-15, which is 30 fps where the game
allows it. Rewriting the backend without citro3d would not: citro3d's own
share is inside the 7-10 ms replay.

## Margin

The budget this has to fit, measured before any code:

- **GPU time.** 14-16 ms a frame for both eyes now, of 33 at 30 fps.
  The new vertex shader runs the N64's math: a matrix from a palette
  (4 `dp4`), the viewport and stereo shift (as now), up to 7 lights (a
  `dp3`, a `max` and 3 `mad` each), fog (a `mad`), texel scale: 20-40
  instructions a vertex against the present 16. At 4,000 vertices and two
  eyes that is some 300,000 instructions a frame; the PICA200's vertex
  unit at 268 MHz does that in about 1-2 ms. Ten times that estimate
  still fits. Phase 0 measures it rather than trusting this paragraph.
- **Uniform registers.** 96 float vectors. The shader uses 8 now. A
  palette of 16 matrices takes 64, the lights 15 (7 directions and
  colours, ambient), fog 1: 88. A model with more bones than the palette
  splits into more draws, as a change of state does now.
- **Memory.** Linear memory has about 6 MB free with the eye targets up.
  A frame's buffers shrink: 4,000 vertices at 20 bytes plus 2,600
  triangles at 6 bytes of indices is about 100 KB, against 220 KB of
  `Vtx3ds` today, double-buffered either way.
- **Precision.** The PICA200's shader floats are 24-bit; the N64's
  matrices are 16.16 fixed point and its coordinates 16-bit, so the
  products fit. Depth is z/w as the GPU already computes it from the
  CPU's floats.
- **The Old 3DS's other core** is untouched: this moves work off core 0,
  nothing onto core 1.
- **The New 3DS keeps the CPU path.** Its GPU is the same chip at the same
  clock, and at 60 fps a frame is 16.7 ms: the 14-16 ms the GPU takes for
  both eyes in a heavy scene is already most of it, with the CPU at 804
  MHz and an L2 doing the transform in time. The row defaults on for an
  Old 3DS and off for a New one; the CPU path is a first-class option,
  not a fallback to retire.

## What the CPU keeps

Each feature that reads transformed positions today, and where it goes:

| today | with the GPU transforming |
|---|---|
| back-face cull, with the eyes' shift turning near triangles' winding (`culled`, `kCullEyes`) | the GPU's cull mode, as the replay sets it now; the shift is in the shader, so the winding the GPU tests is the eye's own |
| the screen cull (`off_screen`) | the rasteriser rejects them; the vertex shader still runs for them, counted in the margin above |
| the guard-band and near-plane clip (`clip_tri`, `kGuard`): the PICA200 clips z only and drops a triangle with a far-off corner whole | a position-only transform on the CPU (4 `dp4` and the band test a vertex, no lighting or texels: 2-3 ms for 4,000 vertices, and only under a perspective camera) flags the vertices outside the band; a triangle with one goes through today's clip path, the rest (96 % in the captures) go as indices. Decided: the clip stays. Dropping such triangles (tried on the captures) loses the floor under the player, a slab beside DK in the outdoor capture, which would flicker with the camera |
| gSPCullDisplayList and G_BRANCH_Z, on a few test vertices | the same position-only transform, on demand for those loads |
| lighting as the RSP's integers (`shade_vertex`) | the shader's floats: a colour off by one in the last bit here and there; dl_test's check gains a tolerance for this path |
| texgen (sphere mapping, `acosf` in the linear case), image-tile and snapshot/frame/render-texture texel mappings (`frame_uv`, `snapshot_uv`, `rtt_uv`) | those draws stay on today's CPU path; a draw record says which path built it |
| frame interpolation: the modelview a load used, its vertices as loaded (`XfEntry`, `FrameLog::vdata`), the in-between re-emitting transformed triangles | simpler: the recorded matrices go into the palette lerped, and the in-between redraws the frame's own index buffers |
| the deferred-triangle and reader-thread split (Banjo's `two_cores_read_on_game_core`) | likely redundant once the read is 12-15 ms; removed in phase 3 if the Banjo numbers agree |
| depth read-back, the depth camera, the stereo classification by projection, the reticle hash | unchanged (they use the GPU's depth and the draw's projection) |
| World distance | its triangle test goes (no per-vertex discard in a PICA vertex shader); the gSPCullDisplayList part stays, or the row is dropped |

## The vertex format

The N64 vertex in RDRAM is 16 bytes (x, y, z, flag, s, t as 16-bit, r g b
a as bytes), stored byte-swapped within 32-bit words as the recompiled
code expects. As built, the interpreter copies it into the CPU path's own
32-byte `Vtx3ds` (x, y, z as floats with w 1, s and t as floats, the colour
bytes, and the palette entry's register in a fourth byte attribute), so
both paths share one attribute layout, one buffer and one shader program;
the planned 20-byte raw copy would save the conversion but cost a layout
switch per path change. Normals share the colour bytes as on the N64; the
shader's lighting reads them signed.

A draw's matrices are a palette: a vertex load under a new matrix takes
the next palette slot, and a draw ends when the palette is full (16) or
the state changes, as it ends on state today. DK64's rap frame loads 414
matrices over about 140 draws.

## Phases

Each phase ends with dl_test pixel-identical (or within the lighting
tolerance) on the capture set, the console bench (`tools/dl_bench_3ds`) run
before and after, and a settings row (`gpu_transform`, off by default
until phase 3) so the console can A/B the two paths in the same session.

0. **Measure the margin** (first, short). A vertex shader with the full
   per-vertex math, benchmarked on the console at 4,000 and 8,000 vertices
   a frame, two eyes: the GPU time it adds, on both consoles (the New
   3DS at 60 fps is the tighter GPU budget). A console breakdown of the
   triangle path (the write-out against the tests) with `dl_bench_3ds`'s
   `DL_BENCH_*` cuts, to confirm the expected saving before the work. If
   the shader costs over 8 ms at 8,000 vertices, or the triangle path's
   write-out is not its bulk, this plan is revised here.
1. **The plain path.** The shader, the vertex format, the palette, the
   position-only band test, for draws that are lit or unlit, textured by
   a tile, with no texgen and no image/snapshot/frame mapping. Every other
   draw falls back per draw to today's code. Interpolation off in this
   phase (the row forces it). The host dl_test runs the shader's math in
   software for the reference image.
2. **Interpolation.** The frame log records palettes and the raw loads;
   the in-between redraws with lerped palettes. Interpolation back on.
3. **The rest.** Texgen in the shader (its non-linear case stays on the
   CPU), the image-tile mapping, Banjo and Goemon's draws, and the
   deferred/reader machinery removed if the Banjo bench says it is no
   longer worth its memory. The row's default turns on.

Not before phase 1 is measured on the console: the CPU path stays the
fallback for every draw kind and the whole frame (the row off) throughout.
