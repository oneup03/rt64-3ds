# Porting an N64ModernRuntime game with rt64-3ds

What a game repository needs, in the order it is set up; then what the
renderer and the platform do, what the console taught them, and how to check
a port. Three games are the reference, each in its repository's
`platform/3ds/`:

- **Donkey Kong 64**: F3DEX2, a 320x240 frame, a camera that reads the depth
  buffer back, and mods written into the game's patches.
- **Banjo-Kazooie (Banjo: Recompiled)**: F3DEX 1.x, a 292x216 frame,
  render-to-texture effects, cutscenes at 20 fps, a frame limiter that waits
  for the RDP every frame, and unmodified `.nrm` mods and ROM-hack game modes
  baked into the executable.
- **Goemon 64 (Mystical Ninja Starring Goemon)**: F3DEX 1.23, 320x240,
  libaudio's `aspMain` audio with a synthesizer the game drives itself (the
  DSP synthesizer below), 2D backdrops and menus that sample the drawn
  frame, and the Old 3DS tuning (asynchronous audio, 16-bit eye buffers).

## Setting up a game repository

1. **Submodule.** Add this repository as `lib/rt64-3ds`. It has a submodule
   of its own (`third_party/GamepadMotionHelpers`): `git submodule update
   --init --recursive`, and say so in the game's README.
2. **Runtime patches.** `nmr-patches/apply.sh lib/N64ModernRuntime` applies
   `nmr-patches/n64recomp/*.patch` inside its `N64Recomp` and
   `nmr-patches/nmr/*.patch` inside the runtime, on a local `3ds-patched`
   branch. It adds whichever patches the checkout lacks (by subject) and
   skips the rest, so a build can run it every time; re-run it on every
   game's runtime when a patch joins the series. The patches add a 3DS
   branch to the runtime and two ways to build its mod system:
   `RECOMP_NO_MODS` (none) or `RECOMP_STATIC_MODS` (baked mods, see "Mods"
   below). Nothing changes for other targets.
3. **Host code generation.** Build `N64RecompCLI`, `RSPRecomp` and
   `file_to_c` on the host and generate `RecompiledFuncs/`,
   `RecompiledPatches/` and the RSP microcode as the desktop build does.
   The patches link with `clang -target mips` and `ld.lld`. Compile the
   recompiled functions, patches and microcode with `-fno-strict-aliasing`
   (see "Audio"). Nothing generated from the ROM goes into git or a CI
   artifact: CI fetches the ROM through a secret and generates inside the
   job.
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
   - The CIA: `add_cia(<target> RSF ${RT64_3DS_DIR}/cia/template.rsf TITLE
     .. PRODUCT .. UNIQUEID 0x..... ICON .. [BANNER_CGFX|BANNER_PNG ..]
     [BANNER_WAV ..])` (`cmake/Cia.cmake`) fills the template's `@TITLE@`,
     `@PRODUCT@` and `@UNIQUEID@` and runs makerom and bannertool; no game
     keeps an RSF of its own. The template asks for `SystemModeExt 124MB`
     (`SystemMode 64MB` on an Old 3DS), `CpuSpeed 804MHz` + `EnableL2Cache`,
     `StackSize 0x80000`, `AffinityMask 3` / `CanAccessCore2` and the
     services `dsp::DSP`, `ptm:sysm`, `nwm::UDS`, `soc:U`. The New 3DS clock
     comes from `osSetSpeedupEnable(true)` at start, CIA and `.3dsx` alike.
     The banner sound must be the project's own: a recording of the game's
     audio (`AUDIO_WAV.TXT`) is the game's asset.
   - `add_3dsx(<target> TITLE .. DESCRIPTION .. ICON ..)` (`cmake/Pica.cmake`)
     makes the `.3dsx` with its SMDH; `add_pica_shader` assembles a `.pica`
     into the target. The toolchain file is `cmake/devkitARM-3ds.cmake`.
   - `tests/*.txt`: input scripts for unattended runs in Azahar
     (`tools/azahar-run.sh --script`; see "Testing in Azahar").
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
   patches' map file, rounded up; `rt64_3ds_check_patch_ram(<target>
   <patches.map>)` turns an overlap into a build error. `RT64_3DS_MEM_SIZE_MB`
   changes the RDRAM model from its 16 MB (nmr patch 0003); the renderer's and
   the audio interpreters' address masks assume 16 MB today, so keep it there.
   `rt64_3ds_check_gpr32(<generated dirs>)` is described under "CPU budget".
8. **Defaults per console.** `GameDesc::menu_defaults` sets the settings
   menu's defaults (a key -> value list applied before `settings.ini` is
   read; an entry marked `MenuDefault::New3dsOnly` or `Old3dsOnly` applies
   only on that console, later entries win). A default of `frame_cap=2`
   ("60 interp.") becomes `GameDesc::old3ds_frame_cap` on an Old 3DS. The
   library's own defaults differ per console too: `log_level` 1 on a New
   3DS and 0 on an Old, `gpu_double_eyes` on only on a New 3DS. Changing a
   default does nothing for a player whose `settings.ini` already has the
   key.
9. **Memory for a big ROM.** `RT64_3DS_LINEAR_HEAP_MB` in the game's
   `CMakeLists.txt` sets the linear heap's size (32 MB with plenty of memory
   and 16 otherwise by default); `GameDesc::rom_room_mb` (24) is the room the
   ROM-in-memory decision wants beside the ROM and RDRAM. Dinosaur Planet's
   64 MB ROM fits a New 3DS's application heap with 16 MB of linear memory
   and 12 MB of room; the log's `heap in use` says what the game takes.
10. **The game's own audio.** `SynthDesc::reserve_channels` leaves ndsp
   channels above the synthesizer's free, and `GameDesc::on_audio_ready`
   is called with the first free one once the audio is up (Dinosaur Planet
   plays its MP3 voice acting there).
11. **Cores.** `GameDesc::two_cores_game_on_core0` and
   `two_cores_read_on_game_core` choose the Old 3DS's two-core layout; see
   "CPU budget". Measure both on the console before choosing.

## Renderer settings

`rt64_3ds::RenderDesc` (`src/renderer/rt64_3ds.h`) describes what the
renderer can't find out from the display lists:

| Field | What sets it |
|---|---|
| `gbi` | `Gbi::F3DEX` for F3DEX/F3DEX 1.x games (Banjo builds its patches with `-DF3DEX_GBI`). The default is F3DEX2. GBI1 commands run in the interpreter's single dispatch: `kF3dexOp` gives them numbers of their own, and each case rewrites the command into F3DEX2's form and jumps to that command's case. |
| `vi_width`, `vi_height` | The game's framebuffer size (DK64 320x240, Banjo 292x216). The frame is placed on the 400x240 screen as RT64 places it: scaled uniformly to the height, centred, with widescreen margins in N64 units. Don't patch the game's framebuffer size. |
| `render_to_texture` | On for games that draw the world into an off-screen colour image and then sample it (Banjo's jiggy transition and picture puzzles). A frame that ends on an off-screen image isn't presented, as on the VI. |
| (snapshots) | Storage buffers the game copies the presented frame into with rectangles are found by themselves (patch RAM, or any off-screen image with `render_to_texture`); up to `kMaxSnapshots` (4) of them at a time, each with a GPU copy of the frame presented when it was stored into (Dinosaur Planet's dissolve blends two, its pause a third; the least recently stored into makes way). `snapshot_buffers` | The RDRAM addresses the game copies the presented frame into with rectangles (DK64's pause background). They're sampled from a copy of the screen. |
| `sample_drawn_frame` | For games that copy strips of the frame just drawn into images with texture rectangles and draw with them (Goemon's menu rims): a copy of each eye's frame is kept for the frames that do it. |
| `force_lod_branch` | `true` (the desktop frontends' setting) always draws the near model of a `gSPBranchLessZ` pair. Set it `false` to keep the game's own level of detail, which is cheaper (Banjo and, since 2026-10-14, DK64 on both consoles). |
| `cull_dl` | `true`: `gSPCullDisplayList` ends a list whose tested vertices all lie off one side of the visible area, as the RSP does (the AND of the vertices' clip codes), with a quarter of the frame's width and height of slack each way for the eyes' shifts and the in-betweens; the object's draws, vertex transforms and texture loads are skipped. The desktop renderer ignores the command, so this was off until 2026-10-14. DK64 tests up to 40 objects a frame: its three captures lost 12-31 % of their draws and 9-29 % of their interpreted commands with the renders pixel-identical (the culled objects were being clipped away triangle by triangle); Banjo never uses it, since its 3DS build keeps the game's own CPU culling on (`culling_patches.c`, the frustum widened 1.25x), while DK64's recomp patches disable the game's for widescreen. The frame line's `culldl N/M` is lists ended / tests. |
| `depth_to_rdram` | Only for games that read the depth buffer on the CPU (DK64's camera). It costs a read-back every frame: the renderer's 8x8 grid is read from the GPU's depth copy (the auto-convergence and the aim depth use it too), then the whole 320x240 RDRAM buffer is filled from it, which is the expensive half (about 2 ms a frame on a New 3DS, three times that at the Old clock, on the renderer's core). `depth_to_rdram_every` fills it every Nth game frame instead (`depth_to_rdram_every_old3ds` is the Old 3DS's value); the grid is still read every frame, so nothing else changes. DK64 sets 2 on an Old 3DS. With `frame_interpolation` it needs `double_eyes`: with one set of eye targets the depth read back would be the in-between's, so every list goes without one. |
| `rules` as depth cameras | Every `World` rule of kind `Any` or `Perspective`, ranges included, names the depth cameras (the depth statistics, auto-convergence, the aim depth, the cut test; up to 8 rules): a game that tags its camera `0x10 + selector` gives `{0x10, 0x1F}`. None: `frame_head_proj_id`. |
| `autoconv` | Tuning for depth-driven auto-convergence: the screen region measured, history length and approach/retreat rates. The defaults are DK64's; Banjo's cutscenes (close-ups low in the frame, cuts at 20 fps) wanted 0.9 / 5 / 0.4 / 1.8. |
| `autoconv.near_clamp` | How near convergence may come, in world units (DK64 4, Goemon 2). |
| `fix_rect_lr`, `merge_rect_slices` | RT64's rect.fixRectLR and the desktop fork's merged widescreen detection: full-screen rectangles drawn to x = 319, or as slices of a row, are stretched over the wide screen. Goemon's backdrops. |
| `scissor_edge_slack` | Scissor edges within this many pixels of the frame's edges are taken as the frame's, so a scissor such as (0,0)-(319,239) counts as covering the width and opens to the wide screen (only a full-width scissor does). Extended scissors (`gEXSetScissor`) are kept as set. 0 by default; Goemon sets 5. |
| `stretch_tint_quads` | A screen tint drawn as triangles, one screen-aligned quad covering the frame to within 12 pixels, untextured or with a texture that repeats both ways (Goemon's Impact battle flashes and smoke: a quad in front of a perspective camera, sized to the 4:3 frame), is stretched to cover the wide screen and kept on the screen plane, as full-screen rectangles are; a texture keeps its texel size, its pattern continuing into the added area. Applied per replayed frame, in-betweens included. Off by default; Goemon sets it. |
| `infinity_rect_cover_zoom` | Scales `Infinity` rectangles about the frame's centre so they still cover the screen after each eye's shift (the desktop's skybox-rect cover zoom). |
| `hud_k_ortho`, `hud_k_persp`, `hud_k_bubble`, `rect_hud_follows_separation`, `snap_hud_shift` | The HUD's depth per kind of draw. Goemon sets the three factors to 2.5 and both switches on: one HUD depth for every kind (-c * separation, as the desktop's `stereoHudNdcOffset`), on whole pixels so text is identical in both eyes. Banjo sets `hud_k_persp` to the desktop's 1.5455. |
| `hud_camera_id`, `primary_world_instance` | A projection group the rules place as HUD (3D HUD items' own camera) that becomes the frame's world when its full-width draws outnumber the world's (a cutscene shot through a camera that passes the game's HUD-camera test); with `primary_world_instance` only one instance of a projection is the world per frame, the others stay HUD (Goemon's menu curtains and rims), and a HUD-camera instance becomes it only with more triangles than the frame's World draws under any scissor (Goemon's pause map draws its world in a window, with two 3D icons through the HUD camera). |
| `unify_world_depth`, `covering_rects_hud` | The world's draws take the depth scale of the nearest of its projections (a close-up camera with its own near plane: Goemon's zoom); every rectangle but backdrops and whole-frame tints takes the HUD shift, including panels filling their own scissor. |
| `merge_image_tiles` | Triangles textured from `LOADTILE` cuts of one wider image sample one texture of the whole image, so draws that differ only in the part they loaded become one. Goemon's sky cylinder (a 32-texel column of its panorama per quad: 280 draws become one). Only where it samples the same texels (TEXEL0 alone, 8/16-bit, coordinates inside the loaded rectangle). Not with the reader's deferred triangles (below), whose compaction renumbers the records. |
| `frame_sizes` | A game whose frame size changes while it runs (Dinosaur Planet: 320x240 in play, 640x480 for its title, menus, game select and name entry, 384x192 once): the sizes it uses. Each list is read at the size the VI's width names at the time (`VI_WIDTH_REG`, matched to one of them), placed on the screen as `vi_width x vi_height` is (scaled to the height, centred); the depth grid's blocks grow with the frame (ceil(w / 50) px); a list read at another size than the one before gets no in-between (`resize` in the reasons). Empty: `vi_width x vi_height` throughout. |
| (presented framebuffers) | The buffers the VI shows are read from its origin register before each list (`note_vi_origin`; the register carries the mode's field offset, so a colour image up to 4 KB below one matches): a list drawing into one is on screen wherever the buffer lives, even above the 8 MB mark that `render_to_texture` otherwise takes as off-screen (Dinosaur Planet keeps its 640x480 front-end buffers in patch RAM), and even when the game names that buffer as its depth image (its 480p mode has no room for one). A list that ends on a storage buffer it only copied the screen into is still on screen. `gEXSetRectAlign` offsets are in the rectangle's own quarter pixels, as RT64 adds them. |
| `inbetween_offscreen` | With `render_to_texture`: the game draws its off-screen images anew every frame (Dinosaur Planet's shadow textures), so a list drawing off screen keeps its in-betweens, which draw their own versions of those images. Off: such a list gets none (`rtt` in the reasons), as an image drawn once and sampled for many frames must not be redrawn. |
| `cut_min_jump` | Frame interpolation: a world camera that moves more than this many units between two lists, and ten times its move before, is a scene cut (no in-between). 5 by default; a fast camera that starts from rest (Dinosaur Planet's Cloudrunner at 20 units a frame) needs a higher floor, 40, or its first move drops an in-between. |
| `aim_texture_hashes` | The aiming reticle by its texture, as the desktop fork finds Dinosaur Planet's (a 32x32 texture rectangle that moves across the screen, so no position test can name it): textured rectangles of 8..64 px, roughly square, have their texels hashed in the read (FNV-1a over the bytes the tile load reads and the palette entries it indexes; `DrawRecord::tex_hash`), and a match is the reticle, placed at the depth under it by the aim sampler (`sample_aim_depth`; its centre is the aim point), with the shift on whole pixels under `snap_hud_shift`. `dbg_aim_tex_log=1` in `settings.ini` logs every candidate once (`aim-tex: <hash> WxH fmt siz tlut`) to collect them in Azahar while aiming. DK64's first-person quad heuristic (`set_first_person`) is unchanged. |
| `frame_interpolation` | Offers "60 interp." on the Frame rate row (`frame_cap=2`): for games whose patches tag their transforms for RT64's high frame rate (`gEXMatrixGroup*`). Goemon, DK64 and Banjo. A game's default of `frame_cap=2` (`GameDesc::menu_defaults`) becomes `GameDesc::old3ds_frame_cap` on an Old 3DS, which can't afford the in-betweens: 1, the 30 fps cap, by default (Goemon), or 0, the game's own rate (DK64, Banjo); `settings.ini` still decides. See "Frame rate". |
| `eye_color16`, `eye_depth16`, `intensity_textures16`, `fill_clear` | The GPU's share of a frame: RGB565 eye colour buffers (snapshots then have no alpha; no dithering, so some banding), 16-bit eye depth, I/IA textures as LA8 (identical texels, half the size), and a frame's leading full-screen fill rectangle (the game's own clear) turned into the eyes' clear colour instead of a screen of fragments. Off by default. All three games set the last three (the console showed Banjo's and DK64's busiest scenes GPU-bound with 3D on; the pictures are the same); none sets `eye_color16`. With `depth_to_rdram`, check that 16-bit depth keeps the game's depth reads working: the write-back converts it. |
| `double_eyes` | With `frame_interpolation`: the in-betweens draw into eye targets of their own (two more 240x400 colour + depth buffers: 1125 KB of VRAM at RGBA8/D16, 1500 KB at D24S8, taken after everything else and skipped when there isn't room), and the game's frame of each pair is recorded into a command buffer of its own while the GPU still draws the in-between; the presenter thread queues it. All three games set it. On an Old 3DS the library leaves it off whatever the game set, since "60 interp." is not its default there (`old3ds_frame_cap`) and the targets would hold the VRAM and a 512 KB command buffer for nothing; `gpu_double_eyes=1` in `settings.ini` turns it on there. See "Double eyes and the presenter". |
| `early_drawn` | The game hears a display list drawn (the RDP's completion) once the renderer has read it, at "parsed", not when the renderer is done with it (nmr patch 0028, `ultramodern::renderer::report_list_drawn`). For a game that waits for the RDP before its next frame (Banjo's frame limiter does, every frame): with frame interpolation the renderer's waits for the screen otherwise come out of the game's frame time, and it misses retraces (Banjo's cutscenes fell behind their music). A list may then wait for the one before; `list_wait_us` says how long. With `depth_to_rdram` the game hears it a little later: once the last game frame's depth is in RDRAM, after the read and once the presenter is done with that frame, since the game must not run while its depth buffer is written. Banjo and DK64 set it. |

## Frame rate

### The 30 fps cap

The player's **Frame rate** row (Game page, `frame_cap` in `settings.ini`)
caps the drawn frames at 30 a second while the game asks for more
(`gEXSetRefreshRate`): every other display list that is a whole frame on the
screen is run for its state only and not drawn, the game and its audio keep
their rate, and the screen keeps the last frame. Lists that draw off screen,
copy the frame or render into textures are always drawn. The stats line says
how many were drawn and passed over, and the fifth-second `frame cap on:`
line how long a passed-over list's state-only run took.

### "60 interp.": in-between frames

With `RenderDesc::frame_interpolation`, the row's third value ("60
interp.", `frame_cap=2`) draws 60 frames a second while the game asks for
less (15 to 59): every display list is drawn as in-between frames from the
list before, one for each refresh of the screen that the game's frame lasts
but its own, then as itself, a refresh apart. At 30 that is one in-between
halfway; at 20 (Banjo's cutscenes) two, at 1/3 and 2/3; at 15 three.

The interpreter records the transforms each list draws with under the
gEXMatrixGroup tags in force (`rt64_3ds_interp.cpp`: RT64's matching by id
in LINEAR order, AUTO by the closest transform, IGNORE never; simple or
decomposed interpolation per component; vertex and tile-scroll
interpolation where the tags ask). Projections are paired by (id,
perspective or ortho, ortho scale bucket), not k-th per tag: DK64's HUD has
two orthos under its camera tag, and one appearing shifted the pairing and
blended the scales. The in-between frame is the list's records with every
triangle's vertices transformed anew by the interpolated matrices
(`Interpreter::build_inbetween`; rectangles, the HUD's 2D, lighting and
state are the game frame's). The in-betweens of one list share its matches:
the first is built with `match_previous(weight)`, the next ones with
`reweight_previous(weight)` while the one before is on the GPU, alternating
between two vertex buffers. Each frame waits until the one before it is on
the screen (the game frame on a presenter thread, so the game does not
wait).

The camera's projection must carry interpolation components (Goemon tags it
with `gEXMatrixGroupSimpleNormal`): a projection tagged SKIP is never
interpolated. The patches decide what is interpolated and what is not
(sprites, cutscene timers, arcade games: DK64's `patches_interpolation.c`
sets skip timers); tagging fixes go in the game's patches, matching,
interpolation and presentation fixes in rt64-3ds.

What the in-betweens can't do:
- A camera cut is detected from the transforms' jump, and can be missed:
  in Goemon one in-between showed the new view after a cut.
- An in-between reuses its game frame's records, so it can't choose another
  level of detail (`gSPBranchLessZ`) or draw what the game frame didn't.
- They draw only: the game's own frames keep their timing, and a game that
  measures its frame rate still measures the game's (see "Game timing").

### The budget and its probes

A list gets fewer in-betweens (the game frame then stays for the refreshes
left) or none when they would not fit the game's frame (all of it, from the
times measured as the game runs; Banjo's console tests of 70 to 100 % found
100 % best once probes ramp up, so the setting and `INTERP_BUDGET.TXT`
went). After 30 lists dropped for the budget, it tries in-betweens anyway
for 8 lists in a row to measure them again. A single try measures high,
because the gfx thread waits out the builder, which in steady running
overlaps the waits for the screen. So during a probe a list that only
waited for the one before keeps its in-between (a resync with the screen
still drops it): otherwise every late list broke the run, and a borderline
scene (Banjo's file select: 25-27 ms a pair running, 31-38 ms alone) never
measured its steady cost again. A run that still doesn't fit doubles the
wait before the next, up to 240 lists.

No in-betweens are drawn when the presents run late (the game's 60 Hz
against the screen's 59.83: a list held back by them, or with `early_drawn`
one that waited more than 40% of the game's frame for the list before),
after a camera cut, or when the frame can't be drawn twice (a screen
snapshot, render to texture, depth written back without `double_eyes`, a
frame ending off screen, the video blanked).

### Double eyes and the presenter

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
time on the game's path (Goemon measured 6-10 ms a list on the console).
With `double_eyes` it is recorded without a frame (its commands into a
buffer of its own, its clears and splits kept in order) while the
in-between still draws, into the game's eye targets (the in-between has its
own, so the depth the auto-convergence reads and the captures stay whole);
the presenter thread then queues it in two frames of citro3d's: its clears
and command lists once the in-between's GPU work is done (the draws run
while the in-between waits for its refresh), and its display transfers once
the in-between is on the screen.

The in-betweens go to the GPU as soon as they are recorded, without their
display transfer (their frame ends with no eye marked used, so citro3d
transfers nothing and asks for no swap): only the transfer, a small frame
of its own, waits for the frame before to be on the screen. Their draws
then run while the gfx thread records the game's frame and while the frame
before waits for its refresh. Before this, the last in-between was held open
on citro3d's stopped GX queue until its slot came, so its draws started only
then; the GPU and the gfx thread took turns and a pair barely fitted two
refreshes (40-46 frames a second on the console in motion, with both cores
30-40% busy). The last in-between's transfer is the presenter's, so the gfx
thread never waits for the screen for it and the list is done a refresh
sooner.

A game that waits for the RDP's completion each frame also needs
`early_drawn` (above): otherwise the presenter's and slot waits, whose
phase drifts against the game's 60 Hz VI timer, come out of the game's
frame, and its seconds cycle between 58-59 and 40-46 frames shown.

### The builder on core 1

On a New 3DS a builder thread on core 1 (below the audio task's thread
there) matches the list with the one before (`match_previous`) and builds
its first in-between (`build_inbetween`) while the gfx thread records the
game's frame. The match cost the gfx thread about 4 ms a list in Banjo's
file select (8,500 vertices). A camera cut, or nothing to move, then shows
only when the gfx thread joins the build, so that list's game frame goes
alone. Lists the budget doesn't afford are still matched on the gfx thread,
because each match carries the transforms over to the next list. So the
game's frame is recorded first, then the in-betweens are drawn, and both go
to the presenter. The GPU still runs the in-betweens first, since it
follows the order they are queued in. That is safe because every frame sets
all of its state at its start: combiner stages, depth, blend, alpha test,
scissor, textures, uniforms, framebuffer. Frames with a capture or
screenshot pending keep the old order. The `frame interpolation timing:` line gives the builds and
what of them the gfx thread waited for.

Moving the list's read to core 1 instead was weighed and not done:
`build_inbetween` uses the interpreter's per-list records and its clipping
state, which the next list's run would overwrite.

### The helper on core 3

A list's build took as long as its read in DK64's heavy scenes (12-18 ms
for 220-450 records), longer than the game frame's recording, so the gfx
thread waited 5-10 ms for it. A helper thread on core 3, which the
application otherwise leaves idle (the system's head tracking also runs
there), builds the second half of each in-between's triangles
(`Interpreter::set_inbetween_helper`; lists of 512 triangles or more). The
triangles are split at the record holding the middle one. The helper has
its own vertex cache, and its output goes into a buffer of its own. The
building thread then copies that buffer into the vertex buffer after its
own output and moves the records' starts, so the frame is byte for byte
the one a single thread builds. `dl_test`'s `DL_SPLIT=<n>` checks that
identity, and is clean under ThreadSanitizer. Only the building thread
writes what the GPU reads, so its flush still covers all of it. Core 3
has been granted to a `.3dsx` from the Homebrew Launcher; when it is
refused, one thread builds as before (the boot log says which:
`a thread on core 3:`). The timing line's `halves built on core 3` gives the
helper's builds, their time, and how long the building thread waited for
them. The touch
screen's status row shows core 3's load as `c3` when the system lets the
application use the core.

### Reading the logs

The renderer's reports need `log_level=2` in `settings.ini` (the New 3DS
default, 1, leaves them out: see "CPU budget", Logging). The stats line
and the lines every fifth second, with `frame_cap=2` and 3D on:

- **`stats:`** `... N shown (I in-between, D without: reasons), game asks R`.
  The reasons: `late` (the presents ran late, or the list waited for the one
  before), `budget`, `snapshot` (the list stored the screen; without
  `double_eyes` only: with it the copy comes out of the game frames' own
  set and an in-between stores nothing, so Dinosaur Planet, which stores
  every frame, still gets its in-betweens), `rtt`, `offscreen`, `blanked`,
  `depth` (`depth_to_rdram` without `double_eyes`), `after60` (the game asked 60 or
  more), `after-unsafe`, `cut`, `untagged`, `diverged` (the match failed).
  `game asks` is what `gEXSetRefreshRate` asked for; log it beside the
  achieved rate, since Banjo asks 20 in many cutscenes.
- **`frame interpolation on:`** the second's frames shown and in-betweens,
  the match and replay cost of each, and the lists without one by reason:
  `late N [M to resync with the screen, the rest arrived while the last one
  was still drawn]`, `over budget N [R runs tried anyway, next after M]`,
  `unsafe`, and the rest of the reasons above.
- **`frame interpolation timing:`** the gfx thread's waits for the screen
  and its time per list (`reading it` is the time to "parsed", what a game
  waiting on it waits for), `lists waited N ms for the one before (max M)`,
  `pair X ms (expected Y)` against the game's frame, `first in-betweens
  matched and built on core 1 N (ms each, ms waited for here)`, `halves
  built on core 3 ...`.
- **`frame interpolation last match:`** `transforms a/b/c (vertices N),
  projections a/b/c, tiles a/b`: matched, in the list, in the list before.
  Walking, turning the camera and jumping in Banjo matched 247/247/247 with
  skinned meshes at `vertices 8-23`.
- **`pairs' gpu waits on the gfx thread: in-between X ms, game frame Y ms
  (N pairs); double eyes on: ...`**: Y should be about 0 with double eyes
  on. The rest: game frames recorded while their in-between drew, what the
  presenter waited for instead, the command buffer's use, and `in-betweens
  transferred by the presenter N, here M` (the last in-between's transfer
  belongs on the presenter).
- **`gpu N ms a frame drawn, M an eye (...); game frames X ms, in-betweens
  Y ms; eyes <formats>, I/IA textures, ghost pass, fill clears`**: console
  only (Azahar's GPU takes about 0.3-0.4 ms an eye). **`fill N screens an
  eye (blended, sampling RGBA32/I/IA textures)`** below it.
- **`frame N: D draws V verts, interp X ms replay Y ms (gpu wait, end,
  depth), tex live N (K KB, V in VRAM, M mipmapped) ...`**: the frame line.
  `interp` is wall time (it includes the in-between's build and the wait
  for the presenter), not CPU time; `end` is `C3D_FrameEnd`.
- **`late frame N: X ms since the previous list (that one: read, tris,
  interp, replay, triangles drained in the read; this one waited ...)`**:
  a list that came more than max(50 ms, frame time + 8) after the one
  before, measured from the game's submissions, so a 20 fps cutscene's 50
  ms gaps don't count.
- **The stereo line** ends with `(the right eye from the left's commands in
  N of M frames)`; **`depth copy check N: ...`** reports the first eight GPU
  depth copies against VRAM.
- **`early depth left to the frame begin for N lists`** (with `early_drawn`
  and `depth_to_rdram`): the last game frame had ended inline or with a plain
  frame end on the presenter, so its draws and depth copy might still have
  been on the GPU; those lists read the depth at the next frame begin and are
  reported drawn when `send_dl` returns, as without `early_drawn`. Expected
  for every list at `frame_cap` 0 and for lists without an in-between.
- **Boot lines:** `in-between builder on (core 1)` or `off (two cores)`,
  `in-betweens' second half on core 3` or `not on core 3 (refused)`,
  `frame interpolation presenter ...`, `double eyes on: in-between eye
  targets N KB (VRAM free A -> B KB), game-frame command buffer N KB`, and
  `list reader up (priority 0x31, core 0), ...` with the reader.

The log cuts lines at 512 characters, which is why the interpolation report
is three lines; a new figure goes on a line of its own.

### Checking an in-between on the host

1. Capture two consecutive lists with `CAPTURE pair` in a test script, or
   `GFX_CAPTURE.TXT` containing `pair <frame>` (`CAPTURE seq N` writes the
   next N frames shown). Captures hold RDRAM: ROM-derived, never in git.
2. Build the tool: `OUT=<path> tools/dl_test/build.sh <game repo>`.
3. `DL_INTERP=1 DL_PREV=<first capture> DL_INTERP_RENDER=inb.ppm dl_test
   <second capture>`, with the game's `DL_GBI=f3dex DL_SCREEN=292x216
   DL_RTT=1` as needed. `DL_INTERP_WEIGHT=` sets the in-between's weight
   (default 0.5), and `DL_REWEIGHT=w` builds the next in-between of the pair
   at `w` the renderer's way, checks it against a fresh match at `w` and
   reports where it puts the vertices between the two frames
   (`DL_REWEIGHT_RENDER=file.ppm` renders it): a reweight equals a fresh
   match to 0.000000, and vertices land at 0.333 / 0.667 of the way.
   `DL_SPLIT=<n>` checks the core-3 split, `DL_DESCRIBE` describes an
   in-between's draws, `DL_RUN_BENCH=1` times the read (with `DL_INTERP=1`
   for the interpolation's records).
4. A glitch that shows in one in-between (a HUD element shrinking for a
   frame): each flat draw's box must lie between its two game frames'
   boxes. Match draws by kind, count, tag and texture, never by index or
   vertex address (sprites come and go, buffers are per frame).
   `dbg_interp_check=1` runs that check on the console; it costs the gfx
   thread every frame, so leave it off otherwise.

### Testing in Azahar

`tools/azahar-run.sh <sd path of .3dsx> -t <seconds> -s 0 -3 1 -d 100 -o
<outdir> --script platform/3ds/tests/<script>.txt`. The script takes a lock
for the whole run (`/var/tmp/azahar-run.lock`): its cleanup closes every
Azahar window, so two sessions' runs closed each other's emulators.

- The game's folder is `~/.var/app/org.azahar_emu.Azahar/data/azahar-emu/
  sdmc/3ds/<sd_dir>/`. Back up its `settings.ini`, `AUTOTEST.TXT`,
  `log.txt` and `saves/` first and restore them afterwards: a `newgame`
  script overwrites a save slot, and a save moved out of the way may still
  be read from its `.bak`.
- Add `frame_cap=2` and `log_level=2` to `settings.ini`.
- Azahar's emulated CPU is slow (core 2 reads 93-100%), so the budget drops
  many in-betweens and the ones drawn are what to judge. Correctness there,
  speed on the console: Azahar does not model the CPU's caches either, so a
  missed cache flush (stale vertices or textures) shows only on the console.
- With `-3 1` screenshots are side by side: crop the left eye. A shot lands
  seconds after its marker, so leave frames before the next input.
- The `stats:` and fifth-second lines above are the check; sample the
  screenshots for both eyes, the pause snapshot and the transitions.

## What the console taught the renderer

### The right eye from the left's commands

The gfx thread recorded every frame twice with 3D on, once for each eye
(1-2.8 ms an eye in DK64's heavy scenes). The right eye now reuses the
left eye's commands. The vertex shader takes the eye's sign as a uniform
(`eye`, x' = x + e(a w + b)), and every class's shift is odd in the eye,
so a draw's commands are the same for both eyes. The left eye's draws
are a command list of their own, set all their state themselves from
emptied caches (`forget_draw_state`), and are queued a second time after
the right eye's target and sign (`close_segment`, `queue_again`). That
leaves the GPU where citro3d thinks it is, apart from what the ghost
pass sets anyway. Frames that draw into other targets between the
eyes' draws (off-screen images, `sample_drawn_frame`'s frame copy) and
the GPU debug mode record both eyes as before.

### Depth from a GPU copy

The game frame's depth (the stereo statistics, the aim sampler,
`depth_to_rdram`) was read by the CPU from VRAM, about a millisecond a
frame for the grid. The GPU now copies the left eye's depth buffer to
linear memory once that eye's draws are queued (`queue_depth_copy`), and
the reads use the copy. The first eight copies, one every 300th frame,
are checked against VRAM in the log (`depth copy check`). `depth_to_rdram` writes the
game's depth buffer a word (two pixels) at a time.

### The list's record

What a list's read records for the in-betweens is kept small, because the
game waits for the read. A triangle stores its three vertices and the index
of a state it shares with its neighbours (`TriState`: its record, tile,
depth and culling). Its texel coordinates are worked out again in the build
from the loaded vertex's own (`LoadedVtx::u/v`), unless they were mapped
onto an image, frame or snapshot. Together with the single dispatch, this
read a list in 9-13% fewer instructions and 16-24% fewer cycles on the PC,
on Banjo and Goemon captures, with identical draws and in-betweens.

Indexed triangle draws were tried: a vertex written once per run of
triangles sharing it, culled before it was written. That cut Banjo's
vertices by 44-47% and Goemon's by 16-27%, with identical pictures, but
saved no time on the console (Banjo 4.3-6.0 ms of triangles a list against
4.4-5.1; Goemon at the Old clock 14.3 against 14.6). The triangle path costs
the same per triangle either way, and the GPU's cost is fill, not vertices.
So the renderer stays non-indexed. The host and Azahar were even on it too:
x86 copies and float maths are cheap, so only the console settles such a
change.

### Culling

Two culls keep off-screen geometry out of the renderer's work, both with
their renders checked pixel-identical on the games' captures (`dl_test`):

- `gSPCullDisplayList` (`RenderDesc::cull_dl`, the table above): the
  game's own bounding-box test, honoured as the RSP does; it skips the
  object's whole list, so the read's commands, matrices, texture lookups
  and vertex transforms. Only a game that issues it benefits (DK64; Banjo
  culls models on the CPU before it builds a list, and its 3DS build keeps
  that culling on with the frustum widened to the screen).
- The screen cull (`Interpreter::set_screen_cull`): a triangle whose three
  vertices lie beyond the same edge of the visible area, by more than a
  slack, can't cover a pixel, so it is dropped before the vertex buffer
  and left out of the in-betweens instead of going to the GPU, which runs
  its vertex shader and rejects it in setup. It needs nothing from the
  game, but it comes after the read's transforms, so it saves vertex-buffer
  writes, GPU vertex work and the in-between builder's share, not the
  read. On the DK64 captures it dropped 23-47 % of the vertices sent to the
  GPU (5-20 % on Banjo's). The visible area is the frame plus the wide
  screen's margins (`ScreenGeometry::x0`): a test against the frame alone
  ate the margins. The slack is a tenth of the frame on an Old 3DS (no
  in-betweens there, and every vertex counts) and a quarter on a New 3DS
  (an in-between may move what the game frame has just off the edge back
  in), never less than twice the eyes' shift at infinity (`sep * 200 px`:
  the world's shift is `sep * 200 * (conv / w - 1)`, so twice it covers
  anything nearer than half the convergence distance); `dbg_screen_cull_pct`
  holds another slack in percent for an A/B on the console,
  `dbg_screen_cull_off` turns it off. The boot log says which.

### Textures in VRAM

Textures go to VRAM, where the GPU reads them faster than from the linear
heap (`TextureCache::set_vram`). They
are decoded into the linear heap as ever. Before a frame begins, the gfx
thread copies up to 8 a frame into VRAM with `C3D_SyncTextureCopy`, each on
a queue of its own. Inside a frame the copies overflowed citro3d's 32-entry
GX queue on Banjo's title, a panic. A texture switches to VRAM only when its
copy runs, so anything recorded earlier still reads the linear copy, which
is freed after the next frame begin. Banjo's Treasure Trove Cove was the
case for it: GPU-bound with 3D on at 9.6 ms an eye against 4.3-6.5 elsewhere
for the same fill (5 screens an eye), so the cost was per fragment, the
texture reads from FCRAM; textures in VRAM took it from 30 to 60.

VRAM is kept free for render targets made later, on demand (colour plus
24-bit depth): 1.5 MB with `render_to_texture` (Banjo's 512x256 jiggy
transition), 640 KB with `sample_drawn_frame` (Goemon's 256x256 menu rims,
which failed 2632 times at the earlier 128 KB), 128 KB otherwise (every
other target is made at start). A target that still finds no VRAM has the
texture cache give some back (`make_vram_room`: textures leave VRAM least
recently used first, freed three frames on), and raises the amount kept free
to that target plus 128 KB; it tries again every four frames while it fails.
When VRAM is short, textures unused for 30 frames make way (retired), and
others stay linear. The frame line's `tex live N (K KB, V in VRAM, M
mipmapped)` gives the split.

### Mipmaps from the game's levels

Mipmaps come from the game's own levels. A draw counts as mipmapped
(`lod_trilerp` in `rt64_3ds_record.h`) when it has G_TL_LOD in 2-cycle mode,
no detail or sharpen texture, and a first cycle that lerps TEXEL0 to TEXEL1
by LOD_FRACTION. The interpreter collects the tiles after the base, up to
G_TEXTURE's level count; each must be exactly half the one before in size
and texel scale, with the same format and palette (`describe_mips`;
`FrameRecord::mips`, `TexDesc::levels`). The texture cache builds a PICA
texture with those levels, down to 8x8 (the PICA's smallest), and the draw
samples it with the mip filter (linear between levels when the tile is
bilinear). The combiner reads TEXEL1 as TEXEL0, so the lerp falls away and
no second texture is sampled. Before this, such draws read both tiles at
every pixel and kept only the first (LOD_FRACTION was a constant 0).
TEXEL1 is still left out.
Banjo's level geometry is drawn this way: 32x32 RGBA16 with 16x16 and 8x8
levels (and a 4x4 the PICA can't use); 19 of 104 draws in a Spiral Mountain
capture, on 10 textures (the other G_TL_LOD draws have level 0 and never
read TEXEL1). DK64's characters are too: faces, eyes and clothing in its
intro, about 33 of 168 draws and 24 textures, all 32x32 RGBA16. Goemon has
none. `dl_test`'s `DL_DUMP` lists each draw's levels. On the console the GPU
time per eye was the same with and without them once textures were in
VRAM; they stay on because they cost nothing and look closer to the N64.

### Palettes

A load that changes TMEM's palette area always means a new palette
snapshot, and a CI draw's cache key hashes the palette entries it reads
(its 16 colours, or all 256; `TexDesc::tlut_hash`), not the load. Banjo
loads the same 16-colour eye palette into slot 4 for one eye and slot 0 for
the other. Keyed on the loaded colours' hash alone, the second load kept a
snapshot whose slot 0 still held the fur palette, the texture cache keyed
on that same hash, and the wrong decode stuck: a brown pupil. Across 18
Banjo and DK64 captures only that capture's eye pixels changed with the fix.

### Cache flushes

Frame ends flush only their own command list (`C3D_FrameEnd(GX_CMDLIST_FLUSH)`,
`C3D_FrameSplit(GX_CMDLIST_FLUSH)` for mid-frame splits): `C3D_FrameEnd(0)`
flushes the whole linear heap with GSPGPU_FlushDataCache, a request to the
GSP module on core 1 that waits for that core. With the builder and a DSP
synth there, that cost several ms a frame on the gfx thread (`end` on the
frame line grew from 3-6 to 7-9 ms, and Banjo's intro fell behind its music
again). What the CPU writes for the GPU is flushed where it is written
instead, with the kernel's `svcFlushProcessDataCache` on the writing core
(`flush_dcache`): the vertices in replay, textures as they are decoded, the
recorded frame's command buffer, the white texture, and the builder's
vertices on core 1 (the reader flushes its vertices from core 0). Code that
adds a buffer the GPU reads must flush it the same way: no frame end will.
The debug-sync frame end keeps the full flush. The bottom screen's console
(status rows, menu, touch buttons, drawn by the CPU into linear memory)
relied on the old full flush: `recomp3ds::screen_flush()` (log.cpp), run on
each main-loop pass, flushes the bottom framebuffer when the console drew.

Every fifth second with "60 interp." a `pairs' gpu waits on the gfx thread:`
line has what the in-between and the game's frame each waited for the GPU
per pair, and with `double_eyes` what the presenter waited instead; compare
`gpu_double_eyes=0` and `1`.

### Captures and dl_test

Check a frame on the host before blaming the console: set
`GFX_CAPTURE.TXT` to a frame number to write that frame's RDRAM and task to
`gfx_task.bin` (or hold SELECT for a second with `dbg_capture=1` in
`settings.ini`, or `CAPTURE` in a test script). One writer thread writes
captures (about 11 s for the 16 MB on the console) while the game plays on;
one asked for meanwhile is refused, and the touch screen says so. Then
`tools/dl_test` renders it with the RDP's own formulas (`DL_GBI=f3dex
DL_SCREEN=292x216 DL_RTT=1 DL_RENDER=out.ppm`; `DL_VI_ORIGINS=a023f4,a983f4`
names the VI origins the log noted, for a game whose presented buffers sit
above the 8 MB mark), `DL_DUMP` lists every draw,
and `DL_TEXDUMP` writes chosen draws' textures (`DL_IMAGE_TILES` for
`merge_image_tiles`, `DL_PASS_OVER` times the frame cap's state-only run,
`DL_DEFER=1` and `DL_DEFER_THREAD=1` the reader's deferrals, `DL_COPY_CHECK=1`
the deferred texture decode). Captures are ROM-derived: keep them in
`/var/tmp` or the build directory, never in git (`/tmp` has a small per-user
quota).

## Game timing

- **A frame measured one retrace short.** The runtime's VI thread shows a
  swap at the retrace (`update_vi`) before the game's VI manager thread has
  counted that retrace (an external message, handled at the game's next
  message call). A game that, after the swap, waits for
  `osViGetCurrentFramebuffer` to equal the next buffer while counting VI
  manager messages (Banjo's frame limiter, `viMgr_func_8024BFD8`) can find
  the swap shown with the retrace uncounted: the frame measures one
  retrace short and the next one long. On the N64 a frame never measures
  fewer retraces than it asked for. Banjo scales physics by the measured
  rate (`time_getDelta`, its camera smoothing, the dialogue box, and the
  Yum-Yum clams' hop: 60 / retraces / 2, so a 30 fps frame measured as one
  retrace doubled their speed and quadrupled their height). Fixed at the
  source in the game's patches (`patches/timing_patches.c`, BK_3DS): a
  frame measuring fewer retraces than it asked for is raised to it, and the
  next long frame gives the retrace back (debt capped at 4); the log says
  how many of the last 1800 frames were raised. It needs the console's
  timing (Azahar showed no short frames). Check whether a new game polls
  the swap: DK64 polls it only in its retrace handler, and Goemon's C does
  not.
- **Attract demos.** Banjo's demos take their physics step from the
  recorded inputs, but the game clock, the demo's exit timer and a few
  others add the actual retraces, so any frame that misses a retrace can
  move a demo out of sync (about once every 10-20 s in the intro at 20
  fps, with lists not waiting for the renderer). It happens in the game's
  own frame-rate mode too: not an interpolation problem. The same property
  makes the demos frame-locked between their transitions, which is what
  the benchmarks below rely on.
- **Waiting for the RDP.** A game whose frame limiter waits for the RDP's
  completion every frame (Banjo) hears it at "parsed" with `early_drawn`
  (above); a game-side attempt to overlap the list's read with the game's
  own work instead gained 2-4 ms a list on the console and introduced a
  freeze (see "Freezes").

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

- `AUDIO_CAPTURE.TXT`: tasks N to N+7 (N from the file, 200 when it names
  none) are written to `audio_task_N.bin` (RDRAM + OSTask, 16 MB each),
  whether the interpreter or the recompiled microcode runs them
  (`dbg_audio_rsp=1` selects the latter). `tools/naudio_test/build.sh <game
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

### The audio task's timing at the Old 3DS clock

- `GameDesc.audio_async` (nmr patch 0026): the audio task reports its
  completion as soon as it is submitted, and its samples reach the AI path
  when it has actually run. An SDK-style audio manager waits for that
  message before it builds the next frame, and the scheduler starts tasks
  only at a retrace, so a 10-17 ms task (the interpreter at 268 MHz) locked
  Goemon's manager to 30 tasks a second: music at half speed with the DSP
  running dry. Only for a game whose samples reach the host through
  `osAiSetNextBuffer` alone. Goemon sets it; Banjo's audio was fine without
  it (3-4 ms a task on core 1, no underruns).
- `GameDesc.audio_margin_frames`: video frames of samples the AI path keeps
  in reserve (Goemon 4). Buffers of tasks still to run count as queued
  before the margin (nmr 0027, `ultramodern::pending_audio_bytes`); without
  that, the manager pinned its smallest frame and the DSP ran dry every
  frame.
- `aspmain_hle_set_low_quality` (aspMain only): half-rate resampling and
  envelope mixing, reverb skipped, about a quarter cheaper. No menu row:
  the DSP synthesizer below replaced it, and with it a per-game cap on the
  synthesizer's physical voices (a hook on the function that fills
  `ALSynConfig`) that Goemon had for the same purpose.

### The DSP synthesizer (Audio engine: DSP)

The interpreter still makes a CPU core run the game's whole audio pipeline.
`GameDesc.synth` instead plays every voice the game's synthesizer starts on
a channel of the 3DS DSP, which resamples and mixes it. The CPU only decodes
each voice's VADPCM straight from the ROM, a few buffers ahead, on a thread
on core 1 (`src/platform/dsp_synth.cpp`). The audio task is skipped, and
the AI path still carries silence: its clock (`audio_frames_played`)
schedules the voices' events, so the music keeps the game's tempo and
latency. A game that sets it plays on the DSP by default; settings.ini's
`audio_engine=0` (no menu row) runs the audio task instead, for
comparisons.

The game's recompiler config hooks its synthesizer's entry points at
function entry. Each hook calls `recomp3ds_synth_hook(rdram, ctx, code)` and
does nothing on the desktop:

```toml
[[patches.hook]]
func = "alSynSetVol"          # n_alSynSetVol in n_audio games
text = """
#ifdef __3DS__
    { extern void recomp3ds_synth_hook(uint8_t*, void*, int); recomp3ds_synth_hook(rdram, ctx, 3); }
#endif
"""
```

| code | libaudio | n_audio |
|---|---|---|
| 0 | `alSynStartVoice(s, v, w)` | `n_alSynStartVoice(v, w)` (DK64 has none) |
| 1 | `alSynStartVoiceParams(s, v, w, pitch, vol, pan, fx, t)` | `n_alSynStartVoiceParams(v, w, pitch, vol, pan, fx, t)` (DK64: three more before t) |
| 2 | `alSynStopVoice` | `n_alSynStopVoice` |
| 3 | `alSynSetVol` | `n_alSynSetVol` |
| 4 | `alSynSetPitch` | `n_alSynSetPitch` |
| 5 | `alSynSetPan` | `n_alSynSetPan` |
| 6 | `alSynSetFXMix` | `n_alSynSetFXMix` |
| 7 | `alAudioFrame` | `n_alAudioFrame` |

`GameDesc.synth` is a `SynthDesc`. `desc.synth = recomp3ds::SynthFlavor::Libaudio`
(or `NAudio`) gives that flavor's stock layout; fields a game's build moved
are set after it:

| field | default | set by |
|---|---|---|
| `synth_global` | — | n_audio: the address of `n_syn` (Banjo 0x80276E84, DK64 0x807563E4) |
| `param_samples`, `cur_samples`, `aux_bus` | 0x1C, 0x20, 0x34 | Banjo: 0x28, 0x2C, 0x40 (Rare's three player slots move the fields 0xC) |
| `param_list`, `lame_list` | 0x2C, 0x14 | Banjo: 0x38, 0x20 |
| `pvoice_offset` | libaudio 0xD8, n_audio 0x88 | the physical voice's steal delay |
| `start_t_stack` | libaudio 28, n_audio 24 | DK64: 36 |
| `fx`, `fx_lowpass` | `Embedded` / `Pointer`, true | DK64: `SynthFx::Dk64`, false |
| `sound_modes` | 0 | DK64: 0x807FF0F0 (mono and headphones move pan) |

What the synthesizers turned out to do, from their compiled code:
- libaudio's envelope mixer **squares** the volume (`vol * vol >> 15`) and
  ramps exponentially; n_audio's takes it as given and ramps linearly.
  Getting this wrong was 6 dB too loud.
- StartVoiceParams' last argument is the volume's ramp time **from
  silence**, not a start delay. A plain start keeps the volume the voice
  had; games set the attack before the start and the decay after it.
- Every call is stamped with `paramSamples` plus the physical voice's steal
  delay. Channels are keyed by the physical voice (`ALVoice + 8`), and a
  call on a voice without one does nothing, as in the game. A start from
  another voice on the same physical voice is a steal: the old sound fades
  first, which no hooked call reports.
- Loops resume at frame `loop.start / 16 + 1` from the loop's saved state.
- Voices can live above the console's 8 MB (Banjo's do): pointer checks use
  the runtime's RDRAM size.

**The command list.** The frame function still builds a command list the
skipped task would run: for Goemon that was 16-31 % of core 0 at the Old
clock, all 16 voices pulled every frame. A second hook, where the frame
function has run the players' handlers (`paramSamples &= ~0xF`) and is
about to enter `while (outLen > 0)`, calls
`recomp3ds_synth_skip_build(rdram, synth or 0, samples left, list end)`.
When that returns non-zero, it has taken the frame's updates as the
envelope mixers would have (stops reset the voice, frees move it to the
lame list, the updates go back to the free list) and moved `curSamples` on.
The hook then sets the samples left to 0 and moves the list's end past the
one command written there. The list must not be empty: Goemon's audio
manager takes an empty list for no task and then never hands the frame's
buffer to the AI, whose clock the DSP's events keep (silence).

| game | `before_vram` | synthesizer | samples left | list end |
|---|---|---|---|---|
| Goemon | 0x80047B40 | s1 | s3 | s4 |
| Banjo | 0x8025C974 | 0 (`n_syn`) | s3 | s6 |
| DK64 | 0x80739B90 | 0 (`n_syn`) | `MEM_W(0x4C, sp)` | `MEM_W(0x34, sp)` |

N64Recomp emits a hook after the branch label at its address, so both paths
into the build loop run it.

Checking a game's synth:
- The `dsp-synth ...` log line (a line of its own each second: the log cuts
  lines at 512 characters) reports:
  - playing channels, starts and events;
  - the decoding thread's `busy` time;
  - `starved` (a channel ran dry) and `ms late` (events applied after their
    time), both 0;
  - `remaps` (the AI clock jumped), 0;
  - `callbacks` (about 204.5 a second);
  - `lists skipped`, which equals the game's audio frames per second.
- Compare loudness and timing with the RSP engine by recording Azahar's own
  audio stream: `pw-dump` gives the stream's `object.serial`, and
  `pw-record --target <serial>` records it.

Not done:
- **Reverb.** libctru's ndsp stores an aux callback but never calls it, and
  never moves the aux bus's samples between the DSP and the CPU. An enabled
  bus mixes its raw sends back into the output. `dsp_reverb.cpp` keeps a
  working effect engine (libaudio and n_audio's sections, with DK64's
  layout) behind `kAuxReturn = false`.
- **Sample-accurate starts.** Events land on DSP frames (4.9 ms).
- **More channels.** There are 23 voice channels; Banjo's 24 and DK64's 30
  physical voices share them, the least recently used going first.

Measured at the Old clock (Goemon): the audio task's 400-700 ms/s on core 1
is gone, the synth costs 8-16 % of core 1, and the audio thread's share of
core 0 dropped from 16-31 % to the game's own sequencer. With the DSP
engine the audio task's thread is idle, and the synth takes 4-6 % of a core
in Banjo.

## CPU budget (Old 3DS)

`dbg_old_clock=1` in `settings.ini` runs a New 3DS at 268 MHz without L2 (the
Old 3DS clock). It still has 3 cores and 124 MB, so it flatters a real Old 3DS.

- **Where the time goes.** The stats line reports:
  - `core0` / `core2` loads (`core1` instead of `core2` with two cores);
  - `ucode ... ms/s` (the audio task);
  - the renderer's `interp` and `replay` per frame (its frame line);
  - per N64 thread, `threads IDLE / MAIN / SCHED / AUDIO / GAME` (nmr 0023,
    `ultramodern::get_thread_loads`). IDLE there is slack (pause_self), not
    CPU, and so is any game thread whose loop is a branch to self: Banjo's
    INIT thread's large share is the game waiting, not working.

  `dbg_profile=1` adds the renderer's split by command kind. The touch
  screen shows frames and each core's load. settings.ini's `dbg_rows=1`
  adds the thread shares and the audio rows. loadmon's calibrations follow
  the clock chosen: 268 and 804 MHz, exactly 1:3.
- **Logging.** settings.ini's `log_level` (log.cpp) has three levels:
  - 2: everything (for tests).
  - 1, the New 3DS default: no renderer reports or late-frame lines, and
    the stats and synth lines only every 10 s.
  - 0, the Old 3DS default (its core 0 is full): boot, warnings and exit
    only.

  A game can set its own default (`GameDesc::menu_defaults`); an entry
  marked `MenuDefault::New3dsOnly` or `Old3dsOnly` applies only on that
  console (later entries win).

  Each line costs a debug-string syscall, a copy and its share of the card
  writes on core 0; `dbg_profile` costs far more (about 18 % in Goemon; it
  reads the clock once per command, about 1.5 us at the Old clock, 16 %
  fewer frames in Banjo, most of it on the triangles, the most numerous
  commands). Azahar buffers its own log and loses the tail
  when azahar-run.sh kills it, so at low levels read the game's `log.txt`
  instead. The log is written by a thread (priority 0x27, above the
  runtime's VI and timer threads, so a spin below it still gets its lines
  onto the card); it is fsynced when a stall alert first fires
  (`recomp3ds::log_sync`), and at boot and exit. Before that, a log that
  "just stopped" proved nothing: the power-off lost its last writes.
- **32-bit registers.** `add_compile_definitions(RECOMP_GPR32=1)` (n64recomp
  0004, nmr 0022) keeps the N64 registers 32-bit when the generated code
  never needs 64; `rt64_3ds_check_gpr32(<generated dirs>)` fails the
  configure step if it does. About 10 % less core-0 time in Goemon. DK64's
  code uses 64-bit operations. Banjo has three hand-written MIPS III
  functions (an 8-byte copy, an 8-byte fill, the checksum's shift register).
  Its 3DS build stubs them in the game's recompilation (Makefile
  `GPR32_STUBS`) and replaces them with 32-bit patches
  (`patches/gpr32_patches.c`; the checksum step matches the original on
  200,000 random seeds); its executable shrank by 18 %. Mods baked into
  the build are compiled with the same definition, so the context layout
  agrees; scan their generated code too.
- **Preemption.** ultramodern delivers external messages (retraces, task
  completions) only inside a message call or the idle thread. A game thread
  computing for long held the scheduler and the audio thread back, and at
  the Old clock Goemon lost half its audio frames. nmr 0025 raises
  `ultramodern_preempt_pending`; Goemon's `TRACE_ENTRY` (include/trace.h,
  run at every function's entry) tests it and calls `ultramodern_preempt`,
  as does Dinosaur Planet's `recomp_enter_function` (its recompiler fork's
  entry hook, platform/3ds/dino_host_protos.h). A game whose recompiled
  code has no such hook gets no preemption: a thread that polls between
  messages then holds every other game thread back for good (Dinomod
  Enhanced's title froze that way until the hook carried the check).
  A game without such a hook needs the recompiler to emit the check (Banjo
  and DK64 have none, so 0025 does nothing for them).
- **Game code at -Os.** No gain (Banjo, Old clock): recompiled code is
  straight-line loads and stores, and shrank only 4.5 %. It stays at -O2.
- **ROM.** `rom_stream` (settings.ini: 0 auto, 1 in memory, 2 from the SD
  card) streams the ROM through a 2 MB cache of 32 KB blocks. Auto streams
  on an Old 3DS or when the heap is short. Every ROM read the runtime makes
  goes through it.

### Freezes, and how to read one

The 3DS does not time-slice: a thread runs until it waits, and threads of
equal priority on a core never take turns. The runtime's host threads run
on the game core at the game threads' priority (0x30), its VI and timer
threads at 0x28, the gfx thread at 0x2C. So a thread that spins at or above
0x30 on core 0 starves everything below it on that core, the runtime's main
thread (0x30) included: no frames, no HOME, and nothing written to the log
unless its writer is above the spinner. Everything found so far was a spin
of that kind, and the console gave no sign of it but a frozen picture with
the sound holding its note. What rt64-3ds adds, so the next one can be
read from a photograph:

- `stall_watch.cpp` runs above the game on core 0 (priority 0x20). Once the
  main loop has started (and not while the HOME prompt holds the game or
  the HOME Menu or sleep has the application), if the main thread misses its
  beat for 3 s it logs
  `recomp3ds: STALL: the main thread has not run for N s; game thread
  running: <name> (for how long); wake-ups since of the others: ...` and
  `STALL over` when it runs again.
- The touch screen's bottom three rows turn red while the main thread or
  the frames have stopped for 3 s (15 s while the game has the video
  blanked: DK64 draws nothing through its loads, 4-7 s on an Old 3DS,
  which lit the row at every load), or the log's card write
  (`log_write_stuck_ms`) for 15 s (a slow card takes over 3 s for some
  writes once the log is a few MB) (not while the HOME Menu or sleep has the screens, not before the
  first frame), drawn from the watcher's own thread with a try-lock and its
  own cache flush:
  - row 29: `STOPPED: main Ns / frames Ns / card write Ns`;
  - row 28, e.g. `0:100 1:9 2:-1 m@sleep g0 RESET 12s`: each core's load
    since the watcher's last check (`loadmon_peek`; near 100 with
    everything stopped means a spinner on that core, near 0 means
    everything waiting), the main thread's stage in its loop (sleep, pad,
    flush, motion, home, select, report, watchdog, apt), the renderer's
    phase (0 idle, 1 interpreting, 2 frame begin, 3 replaying, 4 frame
    end), and the game thread running and for how long (`+N` when more
    than one counts as running; the time comes from 64-bit ticks: the
    32-bit `load_since` wrapped every 16 s and once read `3s` for 51);
  - row 27: the runtime's counts a second (nmr 0030,
    `ultramodern_dbg_counts`): `e` external messages queued, `f` not sent
    (queue full), `r` game thread resumes, `t` timer passes, `v` VI passes,
    `i` idle waits, `w` receive waits, `s` send waits. Normal play is about
    `e~270 f0 r~700 t~150 v60 i~100 w~430 s0`; a requeue spin shows as a
    huge `f` and `i`, a spinning host thread as everything 0.

  The log gets the same lines once per episode, and is fsynced then.
- The main thread's own watchdog logs `recomp3ds: WATCHDOG: no frame for
  4 s` when the renderer stops (a GPU fault: the gfx thread waits forever in
  `C3D_FrameBegin`), so a dead GPU leaves WATCHDOG lines and more stats
  lines; a dead main thread leaves neither.

Reading the signature: a `STALL` line names a game thread (or a host
thread above it) holding core 0; `STOPPED` with core 0 at 100, the runtime
counts at 0 and the main thread `@sleep` (between passes, starved rather
than blocked) is a host thread spinning above the game; `card write` is an
SD write that never returned (an SD card taking 3 s shows the same for a
moment); no red line and a frozen status line is the whole console. The
game thread named is only the one that was current when the spinner took
over.

What has frozen a console so far:
- **The runtime's timer thread** (nmr 0029). It kept the active timers in
  a `std::set` ordered by each OSTimer's own timestamp, read from the
  game's struct at every comparison, which `osSetTimer` rewrites at once
  while its add action waits in the queue. A game that stops and restarts
  the same timers every frame or two (Banjo's audio and controller timers)
  could change a key under the set; an erase then missed and left a fired
  one-shot at the front, fired again forever: core 0 at 100 % at 0x28.
  The thread keeps its own copy of each timer now.
- **moodycamel's `LightweightSemaphore`** (nmr 0031). After a timed wait
  ran out it spun until a `signal()` that had already counted it in
  released the inner semaphore. The timer thread waits with timeouts and
  the game threads below it signal it from `osSetTimer`/`osStopTimer`; a
  timeout landing between `signal()`'s two steps left the timer thread
  spinning forever above the thread that owed the release (minutes to
  hours in). The timed-out waiter blocks on the inner semaphore instead.
  Check any new wait loop on the 3DS for the same pattern: a spin that
  waits for a lower-priority thread on its own core waits forever. A
  stress test in Azahar (game threads sleeping 2 ms inside `signal()`)
  reproduced the console's exact signature and confirmed the fix.
- **A full event queue with `requeue_sp` on.** A game-side change to
  Banjo let `game_draw` queue the PARSED event in a static one-slot queue
  and wait for it later (at guard hooks before the heap and vertex code),
  to overlap the list's read with the game's work. When two game threads
  waited for the same read, the last one to wake cleared the queue, which
  could happen after the main thread had registered the next list: the
  next PARSED event met a full queue, and the runtime's idle loop
  (`pause_self` -> `wait_for_external_message`) retried the delivery and
  requeued it without pause, core 0 at 100 % at 0x30. The overlap gained
  2-4 ms a list on the console and is gone; the game waits for each read
  at once, as the desktop does.
- **A red herring.** Banjo's scheduler answers a fault message with
  `do {} while (1)`, which N64Recomp turns into `pause_self`: it looked
  like a spinner, nothing legitimately sends that message, and a patch
  replacing the loop changed nothing. Read the counts row before patching
  the game.

### Measuring

- Azahar times about one instruction per 268 MHz tick and does not model
  the caches: ratios from its profiles, absolute numbers from the console.
  The host and Azahar were even on changes the console then settled either
  way (indexed triangles, moving the vertex transforms).
- Separate boots of the same build differ by about 2 %, and single scenes
  by up to 10 %; whole-run averages mix content (a run that decoded twice
  the textures hid a 2.7 % gain). Compare the same frame-locked content:
  Banjo's attract demos draw the same number of lists between their
  transitions (`video shown at frame N` in the log; the first eight demos,
  about 11,750-11,790 lists, match across runs), so lists per second over
  demos 0-7 from the cumulative `dl/s` of the stats lines compares builds
  to about 0.2 %. For gains of a few percent, A/B inside one boot: a switch
  file read at a known point (the demo loop's second pass) flips the
  change, and demos 0-7 are compared with 8-15 in one log.
- The console A/B switches are `dbg_*` keys in `settings.ini` (the table
  below); a build of the previous renderer in its own build directory
  bisects what a switch doesn't cover.

### Cores

- On a New 3DS: game threads on core 0, the renderer on core 2, and the
  audio task, the DSP synth's thread and the in-between builder on core 1;
  the in-between helper on core 3 when the system allows it.
- On an Old 3DS (no core 2) the game goes to core 1, and the renderer,
  the audio task and the DSP synth to core 0, the core the system does not
  share. The renderer is the larger load. Goemon at the Old clock with two
  cores: 17.5 fps with the renderer squeezed into core 1's share, 24.4
  swapped (three cores: 26.9).
- With two cores, `GameDesc::two_cores_game_on_core0` puts the game on
  core 0 (whole) and the renderer, audio task and synth on core 1 (its
  80 %). The larger load should get core 0: Goemon's renderer (off), but
  Banjo's game (on), whose core was full at the Old clock (13-20 lists a
  second against 30 asked, core 1 at 75-90 %, core 0 at 55 %). Banjo over
  17 minutes each way: the same frame rate (16.3 and 16.1 lists a second
  where the game asks 20), because its frame is the game's work plus the
  list's read one after the other; but with the game on core 1 it made only
  90 % of its audio and underran 3361 times, against 98 % and 631 on core 0.
  The small threads don't matter: the synth 4-6 % of a core, the audio
  task idle with the DSP engine, the log writer, stall watch, VI and timer
  threads little.
- `GameDesc::two_cores_read_on_game_core` (with the game on core 0): each
  display list's read, up to "parsed", runs on a reader thread on core 0
  while the gfx thread on core 1 waits, then the gfx thread draws it
  (`Settings::read_core`). Not with three cores: tried for Banjo's 60
  interp. on a New 3DS (2026-10-08), the read took 14 ms on core 0
  against 13 on core 2. With interpolation recording the list, the
  vertex transforms stay in the read, and the triangles' hand-over cost
  about what it saved. It suits a game that waits on "parsed" every
  frame (Banjo's `game_draw`): its frame is its own work plus the read, and
  core 0 is idle during the read anyway (the read took 26 ms on core 1
  against about 21 on core 0). A game that does not wait on "parsed"
  should leave it off: there the read on core 1 runs beside the game. The
  reader flushes the vertices it wrote from its own core.
  With the reader on core 0 (2026-10-07), Banjo got 16.8 and 19.2 lists a
  second (from 16.3 and 18.7), core 0 at 95 % and core 1 at 35 %. At first
  the reader ran above the game's threads. Banjo's audio thread then
  waited out each read, the game made 95 % of its audio and it underran
  three times as often. So the reader now runs just below them
  (`Settings::read_priority`, 0x31), as they ran beside the read on core 1.
  It hands the list to the gfx thread before it sends "parsed"; otherwise
  the main thread, woken by "parsed", would hold the reader until its next
  wait. A helper thread's priority is part of the layout: above the game's
  threads it takes the game's time, below them it takes the slack.
- With the reader, the triangles' output is deferred
  (`Interpreter::set_defer_triangles`). The read logs every loaded vertex
  and records each triangle of a plain record as three log entries, with
  one run of state per record and `state_seq`. Then `finish_triangles`
  runs on the gfx thread (core 1) after "parsed": culling, texel
  coordinates, clipping and the vertices. Records that map their texel
  coordinates (onto an image, a frame, a snapshot or a render texture) are
  output in the read as before. It is off with `merge_image_tiles`, whose
  compaction renumbers the records. On the host, Banjo's read got about
  20 % shorter. `DL_DEFER=1` makes `dl_test` defer: the dumps and
  in-betweens match without it.
- The vertex loads are deferred too. The read copies each vertex's 16 RDRAM
  bytes and records its load's context: the matrix, texture scales and fog
  terms, plus for a lit load its lighting (light directions and colours,
  ambient, look-at, kept once while it stays the same). A vertex is
  transformed, lit and fogged when something first uses it
  (`Defer::resolve`). On the reader that means records output in the read,
  vertex edits and the list's last 64 slots. `G_BRANCH_Z` computes the
  depth alone. Everything else waits for `finish_triangles`. Both paths
  share one inline function (`shade_vertex`), so results are
  bit-identical (`dl_test DL_DEFER=1` against `DL_DEFER=tris` and no
  deferral); it must stay inline, or the normal path is 6 % slower. It
  stays off while frame interpolation records the list, which needs the
  lit colours then.
- The gfx thread does the deferred output while the read is still going
  (`Interpreter::drain_triangles`). It would otherwise sit idle, and each
  list goes through it in order: read wait, output, drawing. When that
  sum outgrew the game's frame on Banjo, the next list waited for the
  thread (9.7 ms a late frame), and moving the vertices made it 1 %
  slower. The reader publishes every 128 triangles: the counts are stored
  with release ordering and loaded with acquire, and a callback signals a
  `LightEvent`. The deferral's arrays have a fixed size (about 6.5 MB,
  made once) so nothing moves while both threads use them; when one is
  full, a load is transformed in the read or a triangle is dropped. The
  output goes to `Defer::out` and is copied after the frame's own
  vertices at the end. A vertex both threads resolve is computed twice to
  the same values, and its done flag goes up after the write.
  `dl_test DL_DEFER=1 DL_DEFER_THREAD=1` reads on a second thread: dumps
  identical, and ThreadSanitizer (`libtsan`) reports nothing. With the
  orderings made relaxed it reports 63 races.
- Texture decoding is deferred too (`TextureCache::set_defer_decode`). The
  read still looks up every texture, since the lookup's fingerprint must
  see RDRAM before the game changes it. A texture not in the cache has
  the bytes its decode reads (`texel_span`) and its palette copied, and
  the gfx thread decodes the copies once the read is over
  (`decode_pending`). That is up to ~5 ms a list where a scene changes, and
  little in steady play. `dl_test DL_COPY_CHECK=1` decodes every texture
  from such a copy and from RDRAM and compares: identical on 18 Banjo and
  DK64 captures. Within a list a texture asked for again gets the first
  answer (`TextureCache::begin_list`): Banjo repeats 40-60 % of them.
  Matrices stay in
  the read: about 120 multiplies and 285 combined-matrix updates a list in
  Banjo (~0.5 ms), not worth moving.
- Banjo at the Old clock, timed over the attract demos (see "Measuring"):
  17.94 lists a second with the game on core 0 and no reader; 18.41 with
  the reader; 18.91 with the triangles' output on core 1; 18.73 adding the
  vertices; 19.40 with the output drained during the read (+8.2 %), core 0
  at 98 % and core 1 at 59 %. Core 0 is the limit: the game (about 36-40 ms
  a list), then the read (about 14). Running the game a list ahead (the
  read on core 1, beside the game) would put about 42 ms a list on core 1,
  about 19 lists a second: no gain.
- `settings.ini`'s `dbg_cores` (0 auto, 1 "2, game on 1", 2 "2, game on 0",
  read at start) gives a New 3DS the Old 3DS's layout, to test it on one;
  Auto on an Old 3DS uses the game's choice. The renderer's in-between
  builder (core 1) is off without core 2 (`Settings::no_core2`), and the
  stats line then watches `core1` instead of `core2`.
- Core 1 time: rt64-3ds asks for 80 % and steps down until the process
  manager agrees (it refuses more than the exheader's MaxCpu). A New 3DS
  accepted 90 without an error, as a CIA and as a `.3dsx`, and stayed at
  80, so 80 is the ceiling.
- New 3DS core 3: a `.3dsx` from the Homebrew Launcher could create
  threads there (2026-10-03). loadmon's idle thread got 74 % of core 0's
  count on it, about core 1's share (76 %), and the system used 3-4 %
  during play. The in-between helper runs there (above); a boot-log line
  records whether the application was allowed.

## Files, switches and settings keys

Four files beside the executable (`sdmc:/3ds/<sd_dir>/`) are one-shot
inputs a job needs. They stay files because each would run again at every
start if it were a setting.

| File | Effect |
|---|---|
| `AUTOTEST.TXT` | An input script for unattended runs (`WHEN <log text>` phases, key presses by frame, `SHOT`, `TOUCH`, `CAPTURE pair`/`seq N`, `HOME`, `EXIT`; the format is in `autotest.cpp`); `tools/azahar-run.sh --script` copies one in. |
| `GFX_CAPTURE.TXT` | A frame number, `pair <frame>`, `seq`, `snap`, `rtt`, `seqrtt <count>` or `cam`: writes that frame's RDRAM and task to `gfx_task.bin` for `dl_test` (`seq`: every frame put on the screen). |
| `AUDIO_CAPTURE.TXT` | Writes eight audio tasks from the number inside (default 200) to `audio_task_N.bin` (16 MB each, on the task thread: audio stalls while it writes). |
| `AUDIO_WAV.TXT` | `<start> <length>` in seconds: records the output to `audio.wav` (loudness comparisons; the source for a banner sound, which is then the game's own audio and must not be shipped). |

Every other switch is a `dbg_*` key in `settings.ini`: hidden rows, so the
file carries them all at 0 after the first run; 0 is off, the normal
behaviour; read at start. They are A/B switches and diagnostics for the
console and the host checks, nothing a player needs. Each is also named
where its feature is described. (Until 2026-10-14 these were `*.TXT` files
beside the executable; the settled experiments' switches went: the reader's
layout, textures in VRAM, mipmaps, the core-1 builder, the capture throttle,
the store and snapshot checks, the audio differential mode, the reverb A/B,
the stall dump test and self-test, the audio core choice, the synth trace, and
the early-drawn, eye-replay, depth-copy, build-split and CULLDL A/Bs, and the
`gpu_*` overrides of the eye formats, fill clears, depth interval and LOD.)

| key | effect |
|---|---|
| `dbg_profile` | The `read profile per list:` line (costly: it reads the clock once per command). |
| `dbg_screen_cull_off`, `dbg_screen_cull_pct` | The screen cull off, or its base slack in percent of the frame (`10`, `25`) in place of the console's default; see "Culling". |
| `dbg_interp_check` | Checks each in-between's flat draws against their game frames and logs the ones that land outside (costs the gfx thread every frame). |
| `dbg_no_scissor` | No scissor, to see draws a wrong clip would hide. |
| `dbg_frame_trace_from`, `dbg_frame_trace_count` | A log line per replayed frame from `from` for `count` frames with the replay's decisions (draws recorded and issued, colour image, snapshot store, off-screen, fill skip, eyes, geometry, eye set), to find frames that come out black. |
| `dbg_draw_first`, `dbg_draw_last` | With `last` above 0: replays only those draws of every frame (to find the one that breaks a picture). |
| `dbg_gpu_debug_from`, `dbg_gpu_debug_count`, `dbg_gpu_debug_sync` | With `count` above 0: the GPU debug mode (a sync per draw with `sync` 1, to find the draw that hangs the GPU) for `count` frames from `from`. |
| `dbg_aim_tex_log` | Logs every candidate reticle texture's hash once (`RenderDesc::aim_texture_hashes`). |
| `dbg_old_clock` | A New 3DS at the Old 3DS clock (268 MHz, no L2), applied at start. |
| `dbg_cores` | The two-core layouts an Old 3DS has, on a New 3DS: 1 the game on core 1, 2 on core 0 (read at start). |
| `dbg_slow_gfx_ms`, `dbg_slow_audio_ms` | Pads every display list and audio task by that much, to mimic a slower console in the emulator. |
| `dbg_audio_rsp` | Runs the recompiled audio microcode instead of the interpreter. |

The menu writes every row it knows to `settings.ini`, the hidden ones
included, so after a game's first run the file pins every default: a changed
default (the game's or the library's per-console ones) reaches only new
installs, and a key with no row vanishes at the next save. The file is
written to `settings.ini.part` and moved over (the card's rename refuses an
existing destination, so the old file is removed first; a start that finds
only the `.part` takes it), a second after the last change and at exit. The
library's keys:

| key | default | row | read by |
|---|---|---|---|
| `separation`, `convergence_tenths`, `hud_depth`, `auto_convergence`, `comfort_target`, `stereo_ghost_contrast`, `stereo_ghost_black_floor` | 25, 60, 50, 1, 0, 100, 0 (the games set their own) | 3D page | the gfx thread each frame |
| `stick_deadzone`, `cstick_deadzone`, `cstick_up`, `cstick_camera` | 5, 20, 1, 1 (`cstick_camera` only with `GameDesc::cstick_camera_hold`) | Controls | `input_set_*` |
| `audio` | 1 | Game | `audio_set_enabled` |
| `frame_cap` | 0; the games 2, which an Old 3DS turns into `old3ds_frame_cap` | Game | the gfx thread |
| `dbg_old_clock` | 0 | none, New 3DS only | `set_cpu_speed` (1 runs at the Old 3DS clock) |
| `dbg_cores` | 0 (Auto) | none, read at start | the core layout (1 two cores with the game on core 1, 2 on core 0) |
| `rom_stream` | 0 (auto) | none, read at start | the ROM stream |
| `log_level` | 1 on a New 3DS, 0 on an Old | none | `log_set_level` |
| `dbg_rows`, `dbg_capture` | 0, 0 | none | the touch screen's stats rows; SELECT held for a capture |
| `dbg_*` | 0 | none, read at start | the table above |
| `gpu_double_eyes` | the `RenderDesc` value; off on an Old 3DS | none, read at start | the renderer's init |
| `audio_engine` | 1 (the DSP synth) for a game with `GameDesc::synth` | none | `dsp_synth_set_enabled` |
| `mod.<id>`, `mod.<id>.<option>` | absent = off, the manifest's defaults | Mods | the baked mods, before they load |
| `game_mode` | none | the picker at start | the game-mode picker |

A game's own rows (`GameDesc::menu_options`) are saved the same way.

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
- **`GameDesc.rom_cache_mb` / `rom_block_kb`**: the ROM stream's cache and
  its block, the unit of one card read (2 MB of 32 KB by default). A card
  read costs about 10 ms whatever its size: a game that loads long runs
  (Dinosaur Planet's maps, 244 reads stalling a load for 3 s on a New 3DS)
  wants bigger blocks (128 KB), one that reads small pieces at random
  wants small ones. A miss also queues the next block for a read-ahead
  thread (core 3 on a New 3DS), so a run of reads finds its next block in
  the cache while the game uses the one before. The stats line's
  `rom N reads (M KB) waited T ms in W (max X)` counts the card reads and
  the game's waits in the stream (calls over 50 us), so a slow load can be
  told from a slow card: a 15-minute DK64 session on an Old 3DS made 148
  reads (4.7 MB) in all, about 10 per load, so its 4-7 s loads are not
  waiting on a 2 MB cache; the waits figure says what they wait for.
- **`GameDesc.required_mods`**: mods that are part of the game (Dinosaur
  Planet's Dinomod Enhanced): on whatever `settings.ini` says, no on/off
  row on the Mods page (their options still show), and a message plus exit
  at start when one's file is not in `mods/` (`required_mods_hint` says
  where to get it).
- **`GameDesc.project_version`**: set it to the desktop's version. Mods
  refuse to load against an older one.
- Mods baked in are compiled with the game's `RECOMP_GPR32` setting; a mod
  or game mode that brings 64-bit register code back fails the configure
  check, which names the function.

### Game modes

A `custom_gamemode` mod (a ROM hack as a mod) brings a `patch.bps` whose
output ROM is usually 64 MB, too big for memory beside RDRAM, and the 13-14
MB patch alone ran an Old 3DS out of memory (38 MB of heap as a CIA). So
the console never patches: the player makes the ROM on a PC with
`tools/make_game_mode_rom.py` (standard-library Python), and rt64-3ds
handles the rest when the game is built with static mods:

- Bake the mode's code without `:embed`; the player copies the `.nrm` to
  `mods/`.
- At start, a bottom-screen picker lists the game and each mode found. It
  remembers the last choice.
- `make_game_mode_rom.py <game ROM> <mode .nrm>...` writes
  `modes/<mod id>.z64` (checked against the patch's source and target
  CRCs; a .v64 or .n64 ROM is read as .z64) and `modes/<mod id>.z64.ok`:
  the patch's size and compressed size and the ROM's size. The player
  copies `modes/` next to the game on the SD card.
- The runtime asks the host for the ROM (`recomp::mods::set_rom_file_handler`,
  nmr 0033) and never extracts `patch.bps`. `game_modes.cpp` compares the
  stamp with the `.nrm`'s zip directory (nothing extracted; not the
  patch's CRC32, which is the same for every BPS file since it ends with its
  own CRC), and the game streams its ROM from that file. The picker lists a
  mode whose ROM is missing or made from another `.nrm` in red, with what
  to do, and logs it.
- Saves go to `saves/<mod id>/`.
- Check each built-in code mod against each mode (`mod_bake inspect`): a
  mod that replaces a function a mode also replaces makes that mode fail to
  load, and some mods only make sense on the original game's maps. List
  those in `GameDesc::mods_not_in_game_modes`: they're left out of game
  modes (the player's choice stays for the game) and hidden from the Mods
  page there.
- A mode's content can outrun the console: an area of Jiggies of Time built
  from about 400 separately textured objects (390-550 draws, 12,000 commands
  and 330 palette loads a list) reads in 25-27 ms on core 2 with no single
  hotspot, so even 30 fps is borderline there. Left as the mod's content.
