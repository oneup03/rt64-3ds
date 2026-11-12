/* rt64-3ds display-list markers for a game's patches (3DS builds only).
 *
 * The game's own G_SPNOOP with a magic in its free 24 bits: the N64's
 * microcode and the desktop's RT64 skip it, rt64-3ds reads it. Include after
 * the game's gbi.h (G_SPNOOP is its microcode's).
 *
 * gEX3DSBottomScreen(gfx, dx, dy): the draws that follow go to the touch
 * screen (RenderDesc::hud_on_bottom_supported, while the player has the HUD
 * there), moved by dx, dy N64 pixels (-2048..2047), in the game's own
 * coordinates: the widescreen origins and alignments set for the top screen
 * do not apply there. gEX3DSTopScreen(gfx): back to the top screen. Spans
 * carry into the display lists called inside them and end with the list.
 *
 * gEX3DSBottomBackground(gfx): this frame's touch screen shows the picture
 * the game set with rt64_3ds::set_bottom_background behind its draws
 * (RenderDesc::hud_bottom_background_at_run_time); frames without it show
 * black there. Once anywhere in the frame's lists.
 */
#ifndef RT64_3DS_GBI_H
#define RT64_3DS_GBI_H

#define RT64_3DS_MARKER_MAGIC 0x523344 /* "R3D" */
#define RT64_3DS_MARKER_BOTTOM 1
#define RT64_3DS_MARKER_TOP 2
#define RT64_3DS_MARKER_BACKGROUND 3

#define gEX3DSBottomScreen(pkt, dx, dy)                                                         \
    {                                                                                           \
        Gfx *_g3ds = (Gfx *)(pkt);                                                              \
        _g3ds->words.w0 = ((unsigned int)G_SPNOOP << 24) | RT64_3DS_MARKER_MAGIC;               \
        _g3ds->words.w1 = ((unsigned int)RT64_3DS_MARKER_BOTTOM << 28) |                        \
                          (((unsigned int)(dx) & 0xFFF) << 12) | ((unsigned int)(dy) & 0xFFF);  \
    }

#define gEX3DSTopScreen(pkt)                                                                    \
    {                                                                                           \
        Gfx *_g3ds = (Gfx *)(pkt);                                                              \
        _g3ds->words.w0 = ((unsigned int)G_SPNOOP << 24) | RT64_3DS_MARKER_MAGIC;               \
        _g3ds->words.w1 = (unsigned int)RT64_3DS_MARKER_TOP << 28;                              \
    }

#define gEX3DSBottomBackground(pkt)                                                             \
    {                                                                                           \
        Gfx *_g3ds = (Gfx *)(pkt);                                                              \
        _g3ds->words.w0 = ((unsigned int)G_SPNOOP << 24) | RT64_3DS_MARKER_MAGIC;               \
        _g3ds->words.w1 = (unsigned int)RT64_3DS_MARKER_BACKGROUND << 28;                       \
    }

#endif
