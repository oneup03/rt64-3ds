// N64 buttons on the touch screen: the D-pad and L, which a game's own
// controls may leave out while some mod reads them (Banjo's mods: Banjo
// Bee, Transform Anywhere, Ability Anywhere, Cheats). The game says which
// buttons to offer (touch_pad_set); they are drawn on the bottom screen's
// console under the status lines while the settings menu is closed, and a
// touch holds the button under it (one at a time: the panel reads a single
// point).
#include <3ds.h>
#include <atomic>

#include "recomp3ds.h"
#include "recomp3ds_internal.h"

namespace {

// In console cells (8 x 8 pixels, 0-based): a cross on the left, L on the
// right, between the status lines (rows 0-3) and the HOME prompt (22-25).
// Each direction is 64 x 40 pixels, L 88 x 88: thumb-sized.
struct PadButton {
    uint16_t n64;
    int col, row, w, h;
    const char* label;
};
constexpr PadButton kButtons[] = {
    { recomp3ds::N64_DUP,     9,  5, 8, 5, "Up" },
    { recomp3ds::N64_DLEFT,   1, 10, 8, 5, "Left" },
    { recomp3ds::N64_DRIGHT, 17, 10, 8, 5, "Right" },
    { recomp3ds::N64_DDOWN,   9, 15, 8, 5, "Down" },
    { recomp3ds::N64_L,      28,  7, 11, 11, "L" },
};
constexpr int kFirstRow = 5, kLastRow = 19;

std::atomic<uint16_t> g_offered{ 0 };   // set by the game (any thread)
std::atomic<uint16_t> g_held{ 0 };      // read by the game thread
uint16_t g_drawn_offered = 0, g_drawn_held = 0;
bool g_drawn = false;

void draw(uint16_t offered, uint16_t held) {
    for (int r = kFirstRow; r <= kLastRow; r++) recomp3ds::screen_printf("\x1b[%d;1H%40s", r + 1, "");
    for (const PadButton& b : kButtons) {
        if (!(offered & b.n64)) continue;
        // Blue, yellow while held.
        const char* colour = (held & b.n64) ? "\x1b[43m\x1b[30m" : "\x1b[44m\x1b[37m";
        for (int r = 0; r < b.h; r++) {
            const bool mid = r == (b.h - 1) / 2;
            const int len = (int)__builtin_strlen(b.label);
            const int left = (b.w - len) / 2;
            if (mid) recomp3ds::screen_printf("\x1b[%d;%dH%s%*s%s%*s\x1b[0m", b.row + r + 1, b.col + 1, colour, left, "", b.label, b.w - left - len, "");
            else recomp3ds::screen_printf("\x1b[%d;%dH%s%*s\x1b[0m", b.row + r + 1, b.col + 1, colour, b.w, "");
        }
    }
}

}   // namespace

void recomp3ds::touch_pad_set(uint16_t n64_buttons) {
    g_offered.store(n64_buttons);
}

uint16_t recomp3ds::touch_pad_held() {
    return g_held.load(std::memory_order_relaxed);
}

void recomp3ds::touch_pad_update(bool shown, bool contact, int x, int y, bool redraw) {
    const uint16_t offered = shown ? g_offered.load() : 0;
    uint16_t held = 0;
    if (offered != 0 && contact) {
        const int col = x / 8, row = y / 8;
        for (const PadButton& b : kButtons) {
            if ((offered & b.n64) && col >= b.col && col < b.col + b.w && row >= b.row && row < b.row + b.h) held = b.n64;
        }
    }
    g_held.store(held, std::memory_order_relaxed);
    if (!shown) {
        // The menu owns the screen; draw again once it closes.
        g_drawn = false;
        return;
    }
    // Nothing offered and nothing on screen: leave the console alone (DK64,
    // Banjo without those mods, a message the runtime printed there).
    if (offered == 0 && g_drawn_offered == 0) {
        g_drawn = true;
        return;
    }
    if (redraw || !g_drawn || offered != g_drawn_offered || held != g_drawn_held) {
        draw(offered, held);
        g_drawn = true;
        g_drawn_offered = offered;
        g_drawn_held = held;
    }
}
