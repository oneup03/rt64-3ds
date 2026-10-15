// Buttons on the touch screen for what a game's own controls leave out
// while some mod reads it. Either N64 buttons (touch_pad_set: the D-pad
// and L, DK64's Tag Anywhere) or labelled buttons for what a mod does
// (touch_pad_set_buttons: Banjo's mods), each pressing a set of N64
// buttons with its own timing. They are drawn on the bottom screen's
// console under the status lines while the settings menu is closed, and a
// touch holds the button under it (one at a time: the panel reads a single
// point).
#include <3ds.h>
#include <atomic>
#include <cstdio>

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

// touch_pad_set_buttons: the game's labelled buttons, copied under set_lock()
// (set from any thread, read by the main thread); g_version tells the main
// thread to take the new set.
struct LabelButton {
    char label[18];
    uint16_t press, hold;
    bool right;
    int col, row, w, h;
};
LabelButton g_set[recomp3ds::kTouchButtonsMax];
size_t g_set_count = 0;
int g_set_press_ms = 100;
std::atomic<uint32_t> g_version{ 0 };
// The main thread's copy, and the touch being played out.
LabelButton g_live[recomp3ds::kTouchButtonsMax];
size_t g_live_count = 0;
int g_live_press_ms = 100;
uint32_t g_live_version = 0;
int g_active = -1;              // the button a touch started on
u64 g_active_since = 0;
int g_drawn_active = -2;
bool g_labels_drawn = false;

// Made on first use, from whichever thread gets there first (C++'s guarded
// static initialisation).
LightLock& set_lock() {
    static LightLock lock = [] { LightLock l; LightLock_Init(&l); return l; }();
    return lock;
}

void paint(int col, int row, int w, int h, const char* label, bool held) {
    // Blue, yellow while held.
    const char* colour = held ? "\x1b[43m\x1b[30m" : "\x1b[44m\x1b[37m";
    const int len = (int)__builtin_strlen(label);
    const int left = (w - len) / 2;
    for (int r = 0; r < h; r++) {
        const bool mid = r == (h - 1) / 2;
        if (mid) recomp3ds::screen_printf("\x1b[%d;%dH%s%*s%s%*s\x1b[0m", row + r + 1, col + 1, colour, left, "", label, w - left - len, "");
        else recomp3ds::screen_printf("\x1b[%d;%dH%s%*s\x1b[0m", row + r + 1, col + 1, colour, w, "");
    }
}

void clear_area() {
    for (int r = kFirstRow; r <= kLastRow; r++) recomp3ds::screen_printf("\x1b[%d;1H%40s", r + 1, "");
}

void draw(uint16_t offered, uint16_t held) {
    clear_area();
    for (const PadButton& b : kButtons) {
        if (offered & b.n64) paint(b.col, b.row, b.w, b.h, b.label, (held & b.n64) != 0);
    }
}

// One column of full-width buttons up to four, else two columns, and two
// whenever a button asks for the right one (TouchButton::right: the right
// hand's, which the face buttons leave free). The right column holds those
// first, the left the rest, and what the left can't hold goes on under
// the right's. As tall as the rows between the status lines and the HOME
// prompt allow, with a row between buttons.
void lay_out(LabelButton* b, size_t n) {
    if (n == 0) return;
    size_t want_right = 0;
    for (size_t i = 0; i < n; i++) if (b[i].right) want_right++;
    const int cols = (n <= 4 && want_right == 0) ? 1 : 2;
    int per_col = (int)((n + (size_t)cols - 1) / (size_t)cols);
    if ((int)want_right > per_col) per_col = (int)want_right;
    const int rows = kLastRow - kFirstRow + 1;
    int h = (rows - (per_col - 1)) / per_col;
    if (h > 5) h = 5;
    if (h < 1) h = 1;
    const int w = cols == 1 ? 38 : 19;
    int next[2] = { 0, (int)want_right };   // the next free row of each column (the right's after its own)
    int right_row = 0;
    for (size_t i = 0; i < n; i++) {
        int c = 0, r = 0;
        if (cols == 2 && b[i].right) { c = 1; r = right_row++; }
        else if (cols == 1 || next[0] < per_col) { c = 0; r = next[0]++; }
        else { c = 1; r = next[1]++; }
        b[i].col = cols == 1 ? 1 : (c == 0 ? 0 : 21);     // two columns: 0-18 and 21-39
        b[i].row = kFirstRow + r * (h + 1);
        b[i].w = w;
        b[i].h = h;
    }
}

void draw_labels(int held_index) {
    clear_area();
    for (size_t i = 0; i < g_live_count; i++) {
        const LabelButton& b = g_live[i];
        paint(b.col, b.row, b.w, b.h, b.label, (int)i == held_index);
    }
}

// The labelled buttons' touch: returns the N64 buttons to send now.
uint16_t label_buttons(bool contact, int x, int y) {
    const u64 now = svcGetSystemTick();
    int under = -1;
    if (contact) {
        const int col = x / 8, row = y / 8;
        for (size_t i = 0; i < g_live_count; i++) {
            const LabelButton& b = g_live[i];
            if (col >= b.col && col < b.col + b.w && row >= b.row && row < b.row + b.h) under = (int)i;
        }
    }
    if (g_active >= (int)g_live_count) g_active = -1;
    const u64 press_ticks = (u64)g_live_press_ms * (SYSCLOCK_ARM11 / 1000);
    if (g_active >= 0) {
        // The press runs its full time; after it, the hold lasts while the
        // finger stays on the button.
        if (now - g_active_since < press_ticks) return g_live[g_active].press;
        if (contact && under == g_active) return g_live[g_active].hold;
        g_active = -1;                  // lifted, or slid off: a button it slides onto starts afresh
    }
    if (under >= 0) {
        g_active = under;
        g_active_since = now;
        return g_live[under].press;
    }
    return 0;
}

}   // namespace

void recomp3ds::touch_pad_set_buttons(const TouchButton* buttons, size_t count, int press_ms) {
    LightLock_Lock(&set_lock());
    if (count > kTouchButtonsMax) count = kTouchButtonsMax;
    for (size_t i = 0; i < count; i++) {
        LabelButton& b = g_set[i];
        snprintf(b.label, sizeof(b.label), "%s", buttons[i].label != nullptr ? buttons[i].label : "");
        b.press = buttons[i].press;
        b.hold = buttons[i].hold;
        b.right = buttons[i].right;
    }
    g_set_count = count;
    g_set_press_ms = press_ms > 0 ? press_ms : 100;
    lay_out(g_set, count);
    g_version.fetch_add(1);
    LightLock_Unlock(&set_lock());
}

void recomp3ds::touch_pad_set(uint16_t n64_buttons) {
    g_offered.store(n64_buttons);
}

uint16_t recomp3ds::touch_pad_held() {
    return g_held.load(std::memory_order_relaxed);
}

void recomp3ds::touch_pad_update(bool shown, bool contact, int x, int y, bool redraw) {
    // The game's labelled buttons, when it gave any (touch_pad_set_buttons).
    const uint32_t version = g_version.load();
    if (version != g_live_version) {
        LightLock_Lock(&set_lock());
        g_live_count = g_set_count;
        for (size_t i = 0; i < g_live_count; i++) g_live[i] = g_set[i];
        g_live_press_ms = g_set_press_ms;
        g_live_version = version;
        LightLock_Unlock(&set_lock());
        g_active = -1;
        g_labels_drawn = false;
        g_drawn = false;
    }
    if (g_live_count > 0 || g_labels_drawn) {
        if (!shown) {
            g_held.store(0, std::memory_order_relaxed);
            g_active = -1;
            g_labels_drawn = false;     // the menu owns the screen; draw again once it closes
            return;
        }
        g_held.store(g_live_count > 0 ? label_buttons(contact, x, y) : 0, std::memory_order_relaxed);
        if (g_live_count == 0) {
            clear_area();               // the last labelled buttons went away
            g_labels_drawn = false;
            g_drawn_offered = 0;
            g_drawn = false;
            return;
        }
        if (redraw || !g_labels_drawn || g_active != g_drawn_active) {
            draw_labels(g_active);
            g_labels_drawn = true;
            g_drawn_active = g_active;
        }
        return;
    }
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
