# rt64-3ds

A Nintendo 3DS renderer and platform layer for N64 static recompilations built
on [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime). It fills
the slot that [RT64](https://github.com/rt64/rt64) fills on the desktop: the
game hands it RSP display-list tasks, it draws them, with stereoscopic 3D on
the top screen driven by the hardware slider.

Two libraries:

- `rt64_3ds` — implements `ultramodern::renderer::RendererContext` on
  citro3d: an F3DEX/F3DEX2 display-list interpreter with CPU vertex transform
  and lighting, texture combiner planning, TMEM decoding, textures in VRAM
  with the game's own mipmaps, per-eye replay (the right eye from the left
  eye's commands), and frame interpolation ("60 interp.": an in-between frame
  for each refresh the game's frame lasts, built on the spare cores). The
  stereo code (draw classification, per-eye shift, depth-driven
  auto-convergence) is ported from the owner's RT64 stereo fork.
- `recomp_3ds` — the 3DS entry point a game links: libctru services, threads
  and cores (the Old 3DS's two-core layouts included), audio (CPU
  interpreters of the naudio and aspMain microcode, or the game's voices on
  the DSP), input (hid, gyro), the ROM streamed from the SD card, save and
  config paths, the bottom-screen settings menu and touch buttons, mods baked
  into the executable and ROM-hack game modes, a stall watch, scripted tests,
  and a patch series for N64ModernRuntime and N64Recomp (`nmr-patches/`).

Games on it, each in its repository's `platform/3ds/`: Donkey Kong 64
(github.com/oneup03/Donkey-Kong-64-3D), Banjo-Kazooie
(github.com/oneup03/BanjoRecomp3D) and Goemon 64
(github.com/oneup03/Goemon64Recomp3D). A New 3DS runs them at their full
rate with the interpolated 60; an Old 3DS runs them at the game's own rate on
two cores, with the ROM read from the card.

`docs/porting-a-game.md` is the guide: what a game repository needs, the
renderer and platform options, what the console taught, and how to test.
`tools/` holds the host tools: `dl_test` (replays a captured display list
with the RDP's formulas and checks in-betweens), `tev_test` (the combiner
planner against the RDP), `naudio_test` (the audio interpreters against the
recompiled microcode), `mod_bake` (bakes `.nrm` mods into C) and
`azahar-run.sh` (unattended runs in the Azahar emulator).

Dependencies: devkitARM with libctru and citro3d (devkitPro packages),
the game's own N64ModernRuntime, and GamepadMotionHelpers (a submodule; clone
with `--recursive` or run `git submodule update --init`).

Licence: MIT, the same licence as RT64 (`LICENSE`).
