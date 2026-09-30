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
| `depth_to_rdram` | Only for games that read the depth buffer on the CPU (DK64's camera). It costs a read-back every frame. |
| `autoconv` | Tuning for depth-driven auto-convergence: the screen region measured, history length and approach/retreat rates. The defaults are DK64's. |

Check a frame on the host before blaming the console: set
`GFX_CAPTURE.TXT` to a frame number to write that frame's RDRAM and task to
`gfx_task.bin`. Then `tools/dl_test` renders it with the RDP's own formulas
(`DL_GBI=f3dex DL_SCREEN=292x216 DL_RTT=1 DL_RENDER=out.ppm`), `DL_DUMP`
lists every draw, and `DL_TEXDUMP` writes chosen draws' textures.

## Audio

The RSP audio microcode recompiled by RSPRecomp runs scalar on the ARM11 and
costs most of a core. Set `GameDesc.audio_hle = true` for games whose audio
task is the naudio family (n_aspMain and relatives: F3DEX2-era games such as
DK64 and Banjo); `src/platform/naudio_hle.cpp` then interprets the command
list on the CPU, bit for bit, and the recompiled microcode stays as the
reference for testing only. Keep the game's `get_rsp_microcode` callback as
it is: the platform wraps it.

Verifying a new game's command stream:

- `AUDIO_DIFF.TXT` next to the executable: every command also runs through
  the recompiled microcode and mismatches are logged (`naudio-hle:` lines).
  Rare one-LSB reports in envelope-init cases are the ARM build of the
  microcode disagreeing with the x86 one, not the interpreter.
- `AUDIO_CAPTURE.TXT`: tasks 200-207 are written to `audio_task_N.bin`
  (RDRAM + OSTask, 16 MB each). `tools/naudio_test/build.sh <game repo>`
  builds a host harness that replays them against the x86 microcode.
- The stats line reports `ucode N ms/s in N tasks` and the interpreter's
  command count; `unknown` must stay 0.

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
  player can toggle it and set its options on the settings menu's Mods page.
  Embedded mods start disabled. Mods without `:embed` must also be on the SD
  card in `mods/`: they only run when their file is present.
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
