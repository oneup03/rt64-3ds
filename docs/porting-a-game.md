# Porting an N64ModernRuntime game with rt64-3ds

What a game repository needs, in the order it is set up. Two games are the
reference, both in their repositories' `platform/3ds/`:

- **Donkey Kong 64**: F3DEX2, a 320x240 frame, and mods written into the
  game's patches.
- **Banjo-Kazooie (Banjo: Recompiled)**: F3DEX 1.x, a 292x216 frame,
  render-to-texture effects, and unmodified `.nrm` mods and ROM-hack game
  modes baked into the executable.

1. **Submodule.** Add this repository as `lib/rt64-3ds`.
2. **Runtime patches.** `nmr-patches/apply.sh lib/N64ModernRuntime` applies
   `nmr-patches/n64recomp/*.patch` inside its `N64Recomp` and
   `nmr-patches/nmr/*.patch` inside the runtime, on a local `3ds-patched`
   branch. It skips patches already applied, so a build can run it every
   time. The patches add a 3DS branch to the runtime and two ways to build
   its mod system: `RECOMP_NO_MODS` (none) or `RECOMP_STATIC_MODS` (baked
   mods, see "Mods" below). Nothing changes for other targets.
3. **Host code generation.** Build `N64RecompCLI`, `RSPRecomp` and
   `file_to_c` on the host and generate `RecompiledFuncs/`,
   `RecompiledPatches/` and the RSP microcode as the desktop build does.
   The patches link with `clang -target mips` and `ld.lld`.
4. **`platform/3ds/`** in the game repository:
   - `CMakeLists.txt` that sets `RT64_3DS_NMR_DIR`, adds `lib/rt64-3ds`, builds
     the generated sources, and links `recomp_3ds rt64_3ds`.
   - `main_3ds.cpp`: fill a `recomp3ds::GameDesc` (the `GameEntry` from the
     desktop main, the RSP microcode dispatch, the overlay/patch registration
     functions, and an `rt64_3ds::RenderDesc` holding the game's stereo
     classification rules) and call `recomp3ds::run`.
   - `recomp_api_3ds.cpp`: the game's host functions (`patches/syms.ld`)
     without the desktop launcher's configuration behind them.
   - `<game>_host_protos.h`, force-included into the generated game code,
     declaring any host function the generator calls with an implicit
     declaration.
   - `cia/<game>.rsf` from `cia/template.rsf`, an icon, a banner.
5. **ROM placement.** The runtime looks for `sdmc:/3ds/<sd_dir>/<game_id>.z64`.
6. **Stereo rules.** Port the game's classification from its `rt64-3D`
   branch (`render/rt64_projection_processor.cpp`) into `StereoRule`s.
   `ProjKind::Any`, `Perspective` and `Ortho` rules match triangles. A
   `ProjKind::Rect` rule places the texture and fill rectangles drawn while
   its group id is in force (Goemon's 2D backdrops, at `Infinity`); other
   rectangles follow the HUD depth, or stay on the screen plane when they
   cover their scissor.
7. **Memory.** The recomp heap starts at `0x80A00000` by default. A game whose
   patches' `.bss` ends above that sets `RT64_3DS_HEAP_START` in its
   `CMakeLists.txt` (Banjo: `0x80D00000`), from the end of `.bss` in the
   patches' map file, rounded up.

## Renderer settings

`rt64_3ds::RenderDesc` (`src/renderer/rt64_3ds.h`) describes what the
renderer can't find out from the display lists:

| Field | What sets it |
|---|---|
| `gbi` | `Gbi::F3DEX` for F3DEX/F3DEX 1.x games (Banjo builds its patches with `-DF3DEX_GBI`). The default is F3DEX2. |
| `vi_width`, `vi_height` | The game's framebuffer size (DK64 320x240, Banjo 292x216). The frame is placed on the 400x240 screen as RT64 places it: scaled uniformly to the height, centred, with widescreen margins in N64 units. Don't patch the game's framebuffer size. |
| `render_to_texture` | On for games that draw the world into an off-screen colour image and then sample it (Banjo's jiggy transition and picture puzzles). A frame that ends on an off-screen image isn't presented, as on the VI. |
| `snapshot_buffers` | The RDRAM addresses the game copies the presented frame into with rectangles (DK64's pause background). They're sampled from a copy of the screen. |
| `force_lod_branch` | `true` (the desktop frontends' setting) always draws the near model of a `gSPBranchLessZ` pair. Set it `false` to keep the game's own level of detail, which is cheaper. |
| `depth_to_rdram` | Only for games that read the depth buffer on the CPU (DK64's camera). It costs a read-back every frame. With `frame_interpolation` it needs `double_eyes`: with one set of eye targets the depth read back would be the in-between's, so every list goes without one. |
| `autoconv` | Tuning for depth-driven auto-convergence: the screen region measured, history length and approach/retreat rates. The defaults are DK64's. |
| `fix_rect_lr`, `merge_rect_slices` | RT64's rect.fixRectLR and the desktop fork's merged widescreen detection: full-screen rectangles drawn to x = 319, or as slices of a row, are stretched over the wide screen. Goemon's backdrops. |
| `infinity_rect_cover_zoom` | Scales `Infinity` rectangles about the frame's centre so they still cover the screen after each eye's shift (the desktop's skybox-rect cover zoom). |
| `hud_k_ortho`, `hud_k_persp`, `hud_k_bubble`, `rect_hud_follows_separation`, `snap_hud_shift` | The HUD's depth per kind of draw. Goemon sets the three factors to 2.5 and both switches on: one HUD depth for every kind (-c * separation, as the desktop's `stereoHudNdcOffset`), on whole pixels so text is identical in both eyes. |
| `hud_camera_id` | A projection group the rules place as HUD (3D HUD items' own camera) that becomes the frame's world when its full-width draws outnumber the world's (a cutscene shot through a camera that passes the game's HUD-camera test). |
| `autoconv.near_clamp` | How near convergence may come, in world units (DK64 4, Goemon 2). |
| `merge_image_tiles` | Triangles textured from `LOADTILE` cuts of one wider image sample one texture of the whole image, so draws that differ only in the part they loaded become one. Goemon's sky cylinder (a 32-texel column of its panorama per quad: 280 draws become one). Only where it samples the same texels (TEXEL0 alone, 8/16-bit, coordinates inside the loaded rectangle). |
| `frame_interpolation` | Offers "60 interp." on the Frame rate row (`frame_cap=2`): for games whose patches tag their transforms for RT64's high frame rate (`gEXMatrixGroup*`). Goemon. See below. |
| `eye_color16`, `eye_depth16`, `intensity_textures16`, `fill_clear` | The GPU's share of a frame: RGB565 eye colour buffers (snapshots then have no alpha; no dithering, so some banding), 16-bit eye depth, I/IA textures as LA8 (identical texels, half the size), and a frame's leading full-screen fill rectangle (the game's own clear) turned into the eyes' clear colour instead of a screen of fragments. Off by default (DK64, Banjo); Goemon sets all but `eye_color16`. Each is also a `settings.ini` key read at start (`gpu_color16`, `gpu_depth16`, `gpu_textures16`, `gpu_fill_clear`) for A/B tests on the console. |
| `double_eyes` | With `frame_interpolation`: the in-betweens draw into eye targets of their own (two more 240x400 colour + depth buffers, 1125 KB of VRAM at RGBA8/D16, taken after everything else and skipped when there isn't room), and the game's frame of each pair is recorded into a command buffer of its own while the GPU still draws the in-between; the presenter thread queues it. Off by default; Goemon sets it. Also `gpu_double_eyes` in `settings.ini`. See below. |
| `early_drawn` | The game hears a display list drawn (the RDP's completion) once the renderer has read it, at "parsed", not when the renderer is done with it (nmr patch 0028, `ultramodern::renderer::report_list_drawn`). For a game that waits for the RDP before its next frame (Banjo's frame limiter does, every frame): with frame interpolation the renderer's waits for the screen otherwise come out of the game's frame time, and it misses retraces (Banjo's cutscenes fell behind their music). A list may then wait for the one before; `list_wait_us` says how long. Not with `depth_to_rdram` (that depth is written while the next list is drawn). Banjo sets it. |

The player's **Frame rate** row (Game page, `frame_cap` in `settings.ini`)
caps the drawn frames at 30 a second while the game asks for more
(`gEXSetRefreshRate`): every other display list that is a whole frame on the
screen is run for its state only and not drawn, the game and its audio keep
their rate, and the screen keeps the last frame. Lists that draw off screen,
copy the frame or render into textures are always drawn. The stats line says
how many were drawn and passed over.

With `RenderDesc::frame_interpolation`, the row's third value ("60
interp.", `frame_cap=2`) draws 60 frames a second while the game asks for
less (15 to 59): every display list is drawn as in-between frames from the
list before, one for each refresh of the screen's the game's frame lasts
but its own, then as itself, a refresh apart. At 30 that is one in-between
halfway; at 20 (Banjo's cutscenes) two, at 1/3 and 2/3; at 15 three. The
interpreter records the transforms each list draws with under the
gEXMatrixGroup tags in force (`rt64_3ds_interp.cpp`: RT64's matching by id
in LINEAR order, AUTO by the closest transform, IGNORE never; simple or
decomposed interpolation per component; vertex and tile-scroll
interpolation where the tags ask), and the in-between frame is the list's
records with every triangle's vertices transformed anew by the
interpolated matrices (`Interpreter::build_inbetween`; rectangles, the
HUD's 2D, lighting and state are the game frame's). The in-betweens of one
list share its matches: the first is built with `match_previous(weight)`,
the next ones with `reweight_previous(weight)` while the one before is on
the GPU, alternating between two vertex buffers. Each frame waits until
the one before it is on the screen (the game frame on a presenter thread,
so the game does not wait). A list gets fewer in-betweens (the game frame
then stays for the refreshes left) or none when they would not fit the
game's frame (`INTERP_BUDGET.TXT`: percent of the frame, default 90, from
the times measured as the game runs), and none when the
presents run late (the game's 60 Hz against the screen's 59.83: a list held
back by them, or with `early_drawn` one that waited more than 40% of the
game's frame for the list before), after a
camera cut, or when the frame can't be drawn twice (a screen snapshot,
render to texture, depth written back without `double_eyes`, a frame
ending off screen, the video blanked). The stats line says `N shown (I in-between, D without: reasons)`
and every fifth second a `frame interpolation on:` line has the costs.
The camera's projection must carry interpolation components (Goemon tags it
with `gEXMatrixGroupSimpleNormal`): a projection tagged SKIP is never
interpolated. `dl_test` checks it on the host: `DL_INTERP=1` (with
`DL_PREV=` a capture of the list before, from the test scripts' `CAPTURE
pair`; `CAPTURE seq N` writes the next N frames shown). `DL_INTERP_WEIGHT=`
sets the in-between's weight (default 0.5), and `DL_REWEIGHT=w` builds the
next in-between of the pair at `w` the renderer's way, checks it against a
fresh match at `w` and reports where it puts the vertices between the two
frames (`DL_REWEIGHT_RENDER=file.ppm` renders it).

Every fifth second the log has the GPU's time (`gpu N ms a frame drawn, M
an eye`, game frames and in-betweens apart: citro3d's `C3D_GetDrawingTime`,
from `C3D_FrameEnd` to the queue's last command, so clears, draws, the ghost
pass and the display transfers; meaningful on the console only) and the
fragment load (`fill N screens an eye`, how much of it blends or samples
32-bit textures). `gpu wait` on the frame line is how long the gfx thread
waited in `C3D_FrameBegin` for the GPU.

That wait is citro3d's: it has one command buffer and runs the GX queue
once a frame, so `C3D_FrameBegin` waits until all of the frame before it
(draws and display transfers) is done before a command of the next is
recorded. With frame interpolation the game's frame of a pair is begun just
after its in-between went to the GPU, so it waited about an eye pair of GPU
time on the game's path. With `double_eyes` it is recorded without a frame
(its commands into a buffer of its own, its clears and splits kept in
order) while the in-between still draws, into the game's eye targets (the
in-between has its own, so the depth the auto-convergence reads and the
captures stay whole); the presenter thread then queues it in two frames of
citro3d's: its clears and command lists once the in-between's GPU work is
done (the draws run while the in-between waits for its refresh), and its
display transfers once the in-between is on the screen. The in-betweens
go to the GPU as soon as they are recorded, without their display transfer
(their frame ends with no eye marked used, so citro3d transfers nothing and
asks for no swap): only the transfer, a small frame of its own, waits for
the frame before to be on the screen. Their draws then run while the gfx
thread records the game's frame and while the frame before waits for its
refresh; when the in-between's draws only started once its refresh came
(its frame held open on the stopped GX queue until then), the GPU and the
gfx thread took turns and a pair barely fitted two refreshes (40-46 frames
a second on the console in motion, with both cores 30-40% busy). The last
in-between's transfer is the presenter's, so the gfx thread never waits for
the screen for it and the list is done a refresh sooner. Every fifth second
with "60 interp." a `pairs' gpu waits on the gfx thread:` line has what the
in-between and the game's frame each waited for the GPU per pair, and with
`double_eyes` what the presenter waited instead; compare `gpu_double_eyes=0`
and `1`.

Check a frame on the host before blaming the console: set
`GFX_CAPTURE.TXT` to a frame number to write that frame's RDRAM and task to
`gfx_task.bin` (or hold SELECT for a second with `debug_capture=1` in
`settings.ini`, or `CAPTURE` in a test script). One writer thread writes
captures (about 11 s for the 16 MB on the console) while the game plays on;
one asked for meanwhile is refused, and the touch screen says so. Then `tools/dl_test` renders it with the RDP's own formulas
(`DL_GBI=f3dex DL_SCREEN=292x216 DL_RTT=1 DL_RENDER=out.ppm`), `DL_DUMP`
lists every draw, and `DL_TEXDUMP` writes chosen draws' textures
(`DL_IMAGE_TILES` for `merge_image_tiles`, `DL_PASS_OVER` times the frame
cap's state-only run).

## Audio

The RSP audio microcode recompiled by RSPRecomp runs scalar on the ARM11 and
costs most of a core. Set `GameDesc.audio_hle = true` and the platform
interprets the audio task's command list on the CPU instead, bit for bit,
keeping the recompiled microcode as the reference for testing only. Keep the
game's `get_rsp_microcode` callback as it is: the platform wraps it.
`GameDesc.audio_hle_family` says which microcode the task is, since the two
number and lay out their commands differently:

| Family | Games | Interpreter | Tell-tale |
|---|---|---|---|
| `AudioHle::Naudio` (default) | DK64, Banjo (n_aspMain, F3DEX2 era) | `naudio_hle.cpp` | RSPRecomp output `n_aspMain`; buffers at DMEM 0x4F0 |
| `AudioHle::Aspmain` | Goemon (libaudio's ABI 1 `aspMain`) | `aspmain_hle.cpp` | `abi.h` with `A_SEGMENT 7`, `A_SETBUFF 8`, `A_POLEF 14`; dispatch table at DMEM 0x10, buffers at 0x5C0 |

The aspMain interpreter checks that DMEM 0x000-0x0BF holds the constants of
the build it was written from (Goemon's) and counts `inexact` in the stats
line otherwise, which is the first sign that a game's build differs.

Verifying a new game's command stream:

- `AUDIO_DIFF.TXT` next to the executable: every command also runs through
  the recompiled microcode and mismatches are logged (`naudio-hle:` or
  `aspmain-hle:` lines). The scalar vector unit used to multiply two
  unsigned lanes in `int` (VMUDL, VMADL), which overflows; GCC for the
  ARM11 exploits that, so the console's reference computed Goemon's
  envelope ramps a few LSBs off the desktop's, about ten commands a task
  (DK64's rare one-LSB envelope-init reports are likely the same).
  `nmr-patches/nmr/0021` fixes it: re-run `nmr-patches/apply.sh` on the
  game's runtime before trusting a console diff.
- `AUDIO_CAPTURE.TXT`: tasks N to N+7 (N from the file, 200 when it names
  none) are written to `audio_task_N.bin` (RDRAM + OSTask, 16 MB each),
  whether the interpreter or the recompiled microcode runs them
  (`AUDIO_RSP.TXT` selects the latter). `tools/naudio_test/build.sh <game
  repo>` (`HLE=aspmain` for that family, `SISD=1` for the scalar vector path
  the console runs) builds a host harness that replays them against the x86
  microcode: whole task (all of RDRAM, and DMEM for aspMain), `--lockstep N`
  per command with N mismatches dumped, `--bench N`.
- The stats line reports `ucode N ms/s in N tasks`; `audio-hle unknown N
  mismatches M/T` appears when either is non-zero, `inexact N` when a
  command read a vector the interpreter does not track (an unaligned
  buffer). All must stay 0.

n_aspMain comes in more than one build. DK64's inverts a voice's input when
the low bit of its dry/wet gain is set; Banjo's doesn't.
`GameDesc.audio_hle_variant` picks the build, and a wrong choice cancels
voices against each other: sounds come out quiet or hollow, and the diff
reports it at once.

Whatever the game's toolchain does, compile the recompiled functions,
patches and microcode with `-fno-strict-aliasing`: the desktop builds do,
and without it the scalar vector unit miscompiles.

## Mods

The desktop runtime loads mods by recompiling their code at run time and
writing jumps into native code. Neither works on the console, so rt64-3ds
bakes the mods in when the game is built. The game gets the same mod system
(manifests, config, dependencies, exports, events, hooks and replacements),
fed from tables compiled into the executable.

- **Build.** Set `RT64_3DS_STATIC_MODS` on before adding `lib/rt64-3ds`.
  `tools/mod_bake/build.sh <game repo>` builds the host tool;
  `mod_bake inspect` lists a `.nrm`'s sections, replacements, hooks and
  imports.
- **Bake.** `mod_bake bake <game syms.toml> <patches.elf> <out dir>
  <mod.nrm>[:embed]...` writes each mod's code as C and a table registering
  them all. It also writes a TOML fragment of `[[patches.hook]]` entries:
  the game's and the patches' TOML get those before N64Recomp runs. That
  puts a dispatch check at the entry of every function a baked mod replaces
  or hooks. Compile the output directory's `.c`/`.cpp` files into a library
  linked after the recompiled functions, and call
  `recomp_register_static_mods()` in `main` before `recomp3ds::run`.
  Banjo's `platform/3ds/Makefile` and `CMakeLists.txt` do all of this.
- **`:embed`** puts the `.nrm` itself into the executable (code mods). The
  player switches it and sets its options on the settings menu's Mods page,
  and a switch takes effect at once (every baked code mod is loaded at start,
  each hook and replacement checks its mod's on/off state). The state is
  kept in `settings.ini` (`mod.<id>`, `mod.<id>.<option>`), off until
  switched on; the runtime's `mods.json` is not used. Mods without `:embed`
  must also be on the SD card in `mods/`: they only run when their file is
  present.
- **`GameDesc.on_mod_switched`**: told when a mod is switched on or off, for
  a game whose map leaves out buttons a mod reads. `recomp3ds::touch_pad_set`
  then offers those N64 buttons (the D-pad's directions, L) on the touch
  screen while the settings are closed; a touch holds one. Banjo does that
  by default, and can remap the D-pad and ZL instead ("Mod buttons").
- **`GameDesc.project_version`**: set it to the desktop's version. Mods
  refuse to load against an older one.

### Game modes

A `custom_gamemode` mod (a ROM hack as a mod) brings a `patch.bps` whose
output ROM is usually 64 MB, too big for memory beside RDRAM. rt64-3ds
handles these on its own when the game is built with static mods:

- Bake the mode's code without `:embed`; the player copies the `.nrm` to
  `mods/`.
- At start, a bottom-screen picker lists the game and each mode found. It
  remembers the last choice.
- The first start of a mode writes the patched ROM to
  `sdmc:/3ds/<sd_dir>/modes/<mod id>.z64`, checked against the patch's
  CRCs (about 10 s). Later starts reuse that file, and the game streams its
  ROM from it.
- Saves go to `saves/<mod id>/`.
- Check each built-in code mod against each mode (`mod_bake inspect`): a
  mod that replaces a function a mode also replaces makes that mode fail to
  load, and some mods only make sense on the original game's maps. List
  those in `GameDesc::mods_not_in_game_modes`: they're left out of game
  modes (the player's choice stays for the game) and hidden from the Mods
  page there.
