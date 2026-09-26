# rt64-3ds

A Nintendo 3DS renderer and platform layer for N64 static recompilations built
on [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime). It fills
the slot that [RT64](https://github.com/rt64/rt64) fills on the desktop: the
game hands it RSP display-list tasks, it draws them, with stereoscopic 3D on
the top screen driven by the hardware slider.

Two libraries:

- `rt64_3ds` — implements `ultramodern::renderer::RendererContext` on
  citro3d. The display-list decoder is RT64's own front half (`rt64/`, MIT,
  see `LICENSE.rt64`), trimmed of its GPU back end; the PICA200 side (vertex
  finishing, texture combiner planning, TMEM decoding, per-eye replay) is new.
- `recomp_3ds` — the 3DS entry point a game links: libctru services, audio
  (ndsp), input (hid), save/config paths on the SD card, the bottom-screen
  panel, and a small patch series for N64ModernRuntime (`nmr-patches/`).

Status: under construction. First target is Donkey Kong 64 (New 3DS only).
