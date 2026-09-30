// Small value types shared by the renderer, its interpreter and the host
// tools (no runtime headers).
#ifndef RT64_3DS_TYPES_H
#define RT64_3DS_TYPES_H

#include <cstdint>

namespace rt64_3ds {

// The game's display-list encoding: F3DEX2 (DK64) or F3DEX 1.x, the GBI1
// family (Banjo-Kazooie). The RDP commands are the same in both.
enum class Gbi : uint8_t { F3DEX2, F3DEX };

// Where the game's frame sits on the 400x240 top screen, as RT64 lays it out
// for any window: scaled uniformly to the screen's height, centred, and the
// widescreen margins beside it in the same N64 units (the game's patches
// widen their projection to the screen's 5:3 through
// recomp_get_target_aspect_ratio, and anchor HUD to the edges with gEX
// origins). DK64's 320x240 gives scale 1 and 40-pixel margins; Banjo's
// 292x216 scale 10/9 and 34-unit margins.
struct ScreenGeometry {
    int w = 320, h = 240;          // the N64 frame, N64 pixels
    float scale = 1.0f;            // screen pixels per N64 pixel
    float x0 = 40.0f;              // N64 units from the screen's left edge to N64 x = 0
    float wide_w = 400.0f;         // N64 units across the whole screen: w + 2 * x0
    static ScreenGeometry make(int w, int h) {
        ScreenGeometry g;
        g.w = w > 0 ? w : 320;
        g.h = h > 0 ? h : 240;
        g.scale = 240.0f / (float)g.h;
        g.wide_w = 400.0f / g.scale;
        g.x0 = (g.wide_w - (float)g.w) * 0.5f;
        return g;
    }
    float screen_x(float n64_x) const { return (n64_x + x0) * scale; }
    float screen_y(float n64_y) const { return n64_y * scale; }
};

}   // namespace rt64_3ds

#endif
