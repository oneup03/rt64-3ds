// Buttons on the touch screen, in one grid under the status lines while the
// settings menu is closed:
// - the player's two (the Controls page, touch_pad_set_user): any N64
//   button each, at the top of the right-hand column, ahead of the rest;
// - a game's labelled buttons for what a mod does (touch_pad_set_buttons:
//   Banjo's mods), each pressing a set of N64 buttons with its own timing;
// - or N64 buttons a game's own controls leave out while some mod reads
//   them (touch_pad_set: the D-pad and L, DK64's Tag Anywhere).
// A touch holds the button under it (one at a time: the panel reads a
// single point). With a game's HUD on the touch screen the grid keeps to
// the game's safe area (GameDesc::touch_area_hud) while the buttons fit
// there, else it takes the usual rows over the HUD, its fills see-through.
#include <3ds.h>
#include <algorithm>
#include <atomic>
#include <cstdio>

#include "recomp3ds.h"
#include "recomp3ds_internal.h"
#include "rt64_3ds.h"

namespace {

// In console cells (8 x 8 pixels, 0-based): the rows between the status
// lines (rows 0-3) and the HOME prompt (22-25).
constexpr int kFirstRow = 5, kLastRow = 19;

// Where the grid goes, in cells.
struct Area { int col, row, cols, rows; };
constexpr Area kUsualArea{ 0, kFirstRow, 40, kLastRow - kFirstRow + 1 };
Area g_hud_area{ 0, 0, 0, 0 };          // GameDesc::touch_area_hud (none: no columns)
// The safe area's buttons are at least this tall (cells), for a finger.
constexpr int kHudMinHeight = 2;

// touch_pad_set's buttons, as grid buttons: the D-pad's directions, Right
// and L in the right-hand column (DK64's Tag Anywhere: Left and Right side
// by side).
struct OfferedButton {
    uint16_t n64;
    const char* label;
    bool right;
};
constexpr OfferedButton kOffered[] = {
    { recomp3ds::N64_DUP, "Up", false },
    { recomp3ds::N64_DLEFT, "Left", false },
    { recomp3ds::N64_DRIGHT, "Right", true },
    { recomp3ds::N64_DDOWN, "Down", false },
    { recomp3ds::N64_L, "L", true },
};

struct LabelButton {
    char label[18];
    uint16_t press, hold;
    bool right;                 // wants the right-hand column
    bool user;                  // the player's: the right-hand column's top, ahead of the rest
    int col, row, w, h;
};
constexpr size_t kLiveMax = recomp3ds::kTouchButtonsMax + 2;

std::atomic<uint16_t> g_offered{ 0 };   // touch_pad_set's (any thread)
std::atomic<uint32_t> g_user{ 0 };      // touch_pad_set_user's, first | second << 16 (any thread)
std::atomic<uint16_t> g_held{ 0 };      // read by the game thread

// touch_pad_set_buttons: the game's labelled buttons, copied under set_lock()
// (set from any thread, read by the main thread); g_version tells the main
// thread to take the new set.
LabelButton g_set[recomp3ds::kTouchButtonsMax];
size_t g_set_count = 0;
int g_set_press_ms = 100;
std::atomic<uint32_t> g_version{ 0 };
// The main thread's grid (made from the three sources above), and the touch
// being played out.
LabelButton g_live[kLiveMax];
size_t g_live_count = 0;
int g_live_press_ms = 100;
uint32_t g_live_version = ~0u;
uint16_t g_live_offered = 0;
uint32_t g_live_user = 0;
int g_active = -1;              // the button a touch started on
u64 g_active_since = 0;
int g_drawn_active = -2;
bool g_drawn = false;           // the grid on screen is the live one
bool g_area_used = false;       // something of ours is on screen (to clear)
bool g_live_hud = false;        // the grid was laid out for a HUD on the touch screen
bool g_see_through = false;     // ... over it: its fills see-through (rt64_3ds::set_bottom_console_see_through)
uint16_t g_fill[2] = { 0, 0 };  // the fills' colours as the console paints them (sampled), idle and held

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
    const int first = g_hud_area.cols > 0 ? std::min(kFirstRow, g_hud_area.row) : kFirstRow;
    const int last = g_hud_area.cols > 0 ? std::max(kLastRow, g_hud_area.row + g_hud_area.rows - 1) : kLastRow;
    for (int r = first; r <= last; r++) recomp3ds::screen_printf("\x1b[%d;1H%40s", r + 1, "");
}

// One column of full-width buttons up to four, else two columns, and two
// whenever a button wants the right one (the right hand's, which the face
// buttons leave free). The right column holds the player's buttons first,
// then those that want it, as far as a column goes; the left the rest, and
// what the left can't hold goes on under the right's. As tall as the area's
// rows allow (up to five), with a row between buttons. `cols` 0 chooses as
// above. False when the buttons would be less than min_h tall or a label
// wider than its button (laid out anyway).
bool lay_out(LabelButton* b, size_t n, const Area& area, int min_h, int cols = 0) {
    if (n == 0) return true;
    size_t users = 0, rights = 0;
    for (size_t i = 0; i < n; i++) {
        if (b[i].user) users++;
        else if (b[i].right) rights++;
    }
    if (cols == 0) cols = (n <= 4 && users + rights == 0) ? 1 : 2;
    int per_col = (int)((n + (size_t)cols - 1) / (size_t)cols);
    if ((int)users > per_col) per_col = (int)users;
    int h = (area.rows - (per_col - 1)) / per_col;
    bool fits = h >= min_h;
    if (h > 5) h = 5;
    if (h < 1) h = 1;
    // Two columns of (cols - 2) / 2 with a gap of two, or one inset by a cell.
    const int w = cols == 1 ? area.cols - 2 : (area.cols - 2) / 2;
    for (size_t i = 0; i < n; i++) fits = fits && (int)__builtin_strlen(b[i].label) <= w;
    int slot[kLiveMax][2];      // column, row of each button
    int next[2] = { 0, 0 };
    // The right column: the player's buttons, then those that want it.
    if (cols == 2) {
        for (size_t i = 0; i < n; i++) if (b[i].user) { slot[i][0] = 1; slot[i][1] = next[1]++; }
        for (size_t i = 0; i < n; i++) {
            if (b[i].user || !b[i].right) continue;
            if (next[1] < per_col) { slot[i][0] = 1; slot[i][1] = next[1]++; }
            else slot[i][0] = -1;           // no room: the left column (below)
        }
    }
    // The left column the rest, overflowing under the right's.
    for (size_t i = 0; i < n; i++) {
        const bool placed = cols == 2 && (b[i].user || (b[i].right && slot[i][0] == 1));
        if (placed) continue;
        if (cols == 1 || next[0] < per_col) { slot[i][0] = 0; slot[i][1] = next[0]++; }
        else { slot[i][0] = 1; slot[i][1] = next[1]++; }
    }
    for (size_t i = 0; i < n; i++) {
        b[i].col = cols == 1 ? area.col + 1 : (slot[i][0] == 0 ? area.col : area.col + area.cols - w);   // the usual two: 0-18 and 21-39
        b[i].row = area.row + slot[i][1] * (h + 1);
        b[i].w = w;
        b[i].h = h;
    }
    return fits;
}

// The grid for where it goes now: the usual rows, or with a HUD on the
// touch screen the safe area (one column or two, whichever fits), else the
// usual rows over the HUD, see-through.
void place_live(bool over_hud) {
    g_see_through = false;
    if (over_hud && g_live_count > 0) {
        if (g_hud_area.cols > 0 && (lay_out(g_live, g_live_count, g_hud_area, kHudMinHeight) ||
                                    lay_out(g_live, g_live_count, g_hud_area, kHudMinHeight, 1) ||
                                    lay_out(g_live, g_live_count, g_hud_area, kHudMinHeight, 2))) {
            return;
        }
        g_see_through = true;
    }
    lay_out(g_live, g_live_count, kUsualArea, 1);
}

// The grid from its three sources (main thread).
void make_live(uint16_t offered, uint32_t user) {
    g_live_count = 0;
    for (int k = 0; k < 2; k++) {
        const uint16_t n64 = (uint16_t)(user >> (16 * k));
        if (n64 == 0) continue;
        LabelButton& b = g_live[g_live_count++];
        snprintf(b.label, sizeof(b.label), "%s", recomp3ds::kN64ChoiceNames[recomp3ds::n64_choice_of(n64)]);
        b.press = b.hold = n64;
        b.right = b.user = true;
    }
    LightLock_Lock(&set_lock());
    if (g_set_count > 0) {
        for (size_t i = 0; i < g_set_count && g_live_count < kLiveMax; i++) g_live[g_live_count++] = g_set[i];
        g_live_press_ms = g_set_press_ms;
    }
    else {
        for (const OfferedButton& o : kOffered) {
            if (!(offered & o.n64) || g_live_count >= kLiveMax) continue;
            LabelButton& b = g_live[g_live_count++];
            snprintf(b.label, sizeof(b.label), "%s", o.label);
            b.press = b.hold = o.n64;
            b.right = o.right;
            b.user = false;
        }
        g_live_press_ms = 100;
    }
    LightLock_Unlock(&set_lock());
}

void draw_live(int held_index) {
    clear_area();
    for (size_t i = 0; i < g_live_count; i++) {
        const LabelButton& b = g_live[i];
        paint(b.col, b.row, b.w, b.h, b.label, (int)i == held_index);
        // The fill as painted (a button's top-left pixel: no label there),
        // once of each kind, for the see-through grid.
        uint16_t& fill = g_fill[(int)i == held_index ? 1 : 0];
        if (fill == 0) fill = recomp3ds::screen_pixel(b.col * 8, b.row * 8);
    }
    rt64_3ds::set_bottom_console_see_through(g_see_through ? g_fill[0] : 0, g_see_through ? g_fill[1] : 0);
}

// The touch: returns the N64 buttons to send now.
uint16_t touched_buttons(bool contact, int x, int y) {
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
        b.user = false;
    }
    g_set_count = count;
    g_set_press_ms = press_ms > 0 ? press_ms : 100;
    g_version.fetch_add(1);
    LightLock_Unlock(&set_lock());
}

void recomp3ds::touch_pad_set(uint16_t n64_buttons) {
    g_offered.store(n64_buttons);
}

void recomp3ds::touch_pad_set_user(uint16_t first, uint16_t second) {
    g_user.store((uint32_t)first | ((uint32_t)second << 16));
}

uint16_t recomp3ds::touch_pad_held() {
    return g_held.load(std::memory_order_relaxed);
}

void recomp3ds::touch_pad_set_hud_area(int col, int row, int cols, int rows) {
    g_hud_area = Area{ col, row, cols, rows };
}

void recomp3ds::touch_pad_update(bool shown, bool over_hud, bool contact, int x, int y, bool redraw) {
    const uint32_t version = g_version.load();
    const uint16_t offered = g_offered.load();
    const uint32_t user = g_user.load();
    if (version != g_live_version || offered != g_live_offered || user != g_live_user || (shown && over_hud != g_live_hud)) {
        make_live(offered, user);
        if (shown) g_live_hud = over_hud;
        place_live(g_live_hud);
        g_live_version = version;
        g_live_offered = offered;
        g_live_user = user;
        g_active = -1;
        g_drawn = false;
    }
    if (!shown) {
        // The menu owns the screen; draw again once it closes.
        g_held.store(0, std::memory_order_relaxed);
        g_active = -1;
        g_drawn = false;
        g_area_used = false;
        return;
    }
    g_held.store(g_live_count > 0 ? touched_buttons(contact, x, y) : 0, std::memory_order_relaxed);
    if (g_live_count == 0) {
        // Nothing to show: clear what was there once, then leave the console
        // alone (a message the runtime printed there).
        if (g_area_used) clear_area();
        g_area_used = false;
        if (g_see_through) rt64_3ds::set_bottom_console_see_through(0, 0);
        g_see_through = false;
        g_drawn = true;
        return;
    }
    if (redraw || !g_drawn || g_active != g_drawn_active) {
        draw_live(g_active);
        g_drawn = true;
        g_area_used = true;
        g_drawn_active = g_active;
    }
}
