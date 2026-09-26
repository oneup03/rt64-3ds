# Porting an N64ModernRuntime game with rt64-3ds

What a game repository needs, in the order it is set up. Donkey Kong 64
(`platform/3ds/` in that repository) is the reference.

1. **Submodule.** Add this repository as `lib/rt64-3ds`.
2. **Runtime patches.** Apply `nmr-patches/n64recomp/*.patch` inside
   `lib/N64ModernRuntime/N64Recomp` and `nmr-patches/nmr/*.patch` inside
   `lib/N64ModernRuntime` (`git apply --3way`). They add a 3DS branch to the
   runtime and a `RECOMP_NO_MODS` option; nothing changes for other targets.
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
