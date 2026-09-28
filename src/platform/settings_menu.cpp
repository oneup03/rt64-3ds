// The settings menu on the touch screen. A tap of SELECT opens and closes it
// (holding SELECT takes a frame capture instead, see recomp3ds_run.cpp).
// While it is open the D-pad chooses a row (up/down) and changes it
// (left/right, held to repeat) and the game does not see the D-pad; the
// [-]/[+] buttons work by touch as well. Values go to <base>/settings.ini a
// second after the last change.
//
// The stereo rows come first: the 3D slider sets the strength, these its
// shape (the owner's desktop settings, same units and defaults). The game's
// own options (GameDesc::menu_options) follow.
#include <3ds.h>
#include <cstdio>
#include <cstring>

#include "rt64_3ds.h"
#include "recomp3ds.h"
#include "recomp3ds_internal.h"

namespace {

char g_path[160] = "";
bool g_open = false, g_dirty = false, g_redraw = true;
u64 g_changed_at = 0;
int g_sel = 0;

// Console layout (40 x 30 characters): the game's name on row 0 and the
// stats on row 2 belong to recomp3ds_run.cpp, as do the HOME prompt (rows
// 23-26) and the capture message (row 28).
constexpr int kHintRow = 4, kHeaderRow = 6, kFirstRow = 8, kRowStep = 2, kLastRow = 21;
constexpr int kMinusCol = 16, kValueCol = 22, kValueWidth = 11, kPlusCol = 35;

struct Row {
    const char* key;
    const char* label;
    int lo, hi;
    int* value;
    const char* const* names;       // shown instead of the number; stepping wraps
    bool tenths;                    // shown as n.n
    void (*on_change)(int value);
};
constexpr int kMaxRows = 7;
Row g_rows[kMaxRows];
int g_row_count = 0;

// Convergence is kept in hundredths (0.2 world units each) but shown and
// stepped in tenths like the desktop slider; auto-convergence is a bool.
int g_conv_tenths = 60;
int g_auto = 1;
const char* const kOffOn[] = { "off", "on" };

void apply_conv(int v) { rt64_3ds::settings().convergence_hundredths = v * 10; }
void apply_auto(int v) { rt64_3ds::settings().auto_convergence = v != 0; }

void add_row(const Row& r) {
    if (g_row_count < kMaxRows) g_rows[g_row_count++] = r;
}

int row_line(int i) { return kFirstRow + i * kRowStep; }

void draw_row(int i) {
    const Row& r = g_rows[i];
    char val[24];
    if (r.names != nullptr) snprintf(val, sizeof(val), "%s", r.names[*r.value - r.lo]);
    else if (r.tenths) snprintf(val, sizeof(val), "%d.%d", *r.value / 10, *r.value % 10);
    else snprintf(val, sizeof(val), "%d", *r.value);
    // Centred in its field.
    const int len = (int)strlen(val) < kValueWidth ? (int)strlen(val) : kValueWidth;
    const int left = (kValueWidth - len) / 2;
    const bool sel = i == g_sel;
    printf("\x1b[%d;0H%s%s %-12s\x1b[0m  [ - ] %*s%-*s  [ + ]", row_line(i), sel ? "\x1b[33m" : "", sel ? ">" : " ", r.label,
           left + len, val, kValueWidth - left - len, "");
}

void blank_rows(int from, int to) {
    for (int r = from; r <= to; r++) printf("\x1b[%d;0H%40s", r, "");
}

void draw_hint() {
    printf("\x1b[%d;0H%-40s", kHintRow, g_open ? "SELECT: close   D-Pad: choose, change" : "SELECT: settings");
}

void draw() {
    blank_rows(kHintRow - 1, kLastRow);
    draw_hint();
    if (!g_open) return;
    printf("\x1b[%d;0H--------------- Settings ---------------", kHeaderRow);
    for (int i = 0; i < g_row_count; i++) draw_row(i);
    const int after = row_line(g_row_count - 1) + kRowStep;
    if (after + 1 <= kLastRow) {
        printf("\x1b[%d;0HThe 3D slider sets the strength.", after);
        printf("\x1b[%d;0HHUD depth 50 = on the screen.", after + 1);
    }
}

void save() {
    FILE* f = fopen(g_path, "w");
    if (f == nullptr) return;
    for (int i = 0; i < g_row_count; i++) fprintf(f, "%s=%d\n", g_rows[i].key, *g_rows[i].value);
    fclose(f);
}

void load() {
    FILE* f = fopen(g_path, "r");
    if (f == nullptr) return;
    char line[96];
    while (fgets(line, sizeof(line), f)) {
        char key[48];
        int v;
        if (sscanf(line, "%47[^=]=%d", key, &v) != 2) continue;
        for (int i = 0; i < g_row_count; i++) {
            const Row& r = g_rows[i];
            if (strcmp(key, r.key) == 0) *r.value = v < r.lo ? r.lo : v > r.hi ? r.hi : v;
        }
    }
    fclose(f);
}

void changed() {
    g_dirty = true;
    g_changed_at = svcGetSystemTick();
}

void step(int i, int d) {
    Row& r = g_rows[i];
    int v = *r.value + d;
    if (r.names != nullptr) v = v < r.lo ? r.hi : v > r.hi ? r.lo : v;
    else v = v < r.lo ? r.lo : v > r.hi ? r.hi : v;
    if (v == *r.value) return;
    *r.value = v;
    if (r.on_change != nullptr) r.on_change(v);
    changed();
    draw_row(i);
}

void select_row(int i) {
    if (i == g_sel || i < 0 || i >= g_row_count) return;
    const int old = g_sel;
    g_sel = i;
    draw_row(old);
    draw_row(i);
}

}   // namespace

void recomp3ds::settings_menu_init(const char* base_path, const GameDesc& desc) {
    snprintf(g_path, sizeof(g_path), "%s/settings.ini", base_path);
    rt64_3ds::Settings& st = rt64_3ds::settings();
    g_conv_tenths = st.convergence_hundredths / 10;
    g_auto = st.auto_convergence ? 1 : 0;
    g_row_count = 0;
    add_row({ "separation", "3D depth", 0, 100, &st.sep_slider, nullptr, false, nullptr });
    add_row({ "convergence_tenths", "Convergence", 1, 200, &g_conv_tenths, nullptr, true, apply_conv });
    add_row({ "hud_depth", "HUD depth", 0, 100, &st.hud_depth, nullptr, false, nullptr });
    add_row({ "auto_convergence", "Auto conv.", 0, 1, &g_auto, kOffOn, false, apply_auto });
    add_row({ "comfort_target", "Comfort", -20, 30, &st.comfort_target, nullptr, false, nullptr });
    for (size_t i = 0; i < desc.menu_option_count; i++) {
        const MenuOption& o = desc.menu_options[i];
        add_row({ o.key, o.label, o.lo, o.hi, o.value, o.names, false, o.on_change });
    }
    load();
    for (int i = 0; i < g_row_count; i++) {
        if (g_rows[i].on_change != nullptr) g_rows[i].on_change(*g_rows[i].value);
    }
    fprintf(stderr, "recomp3ds: settings:");
    for (int i = 0; i < g_row_count; i++) fprintf(stderr, " %s %d", g_rows[i].key, *g_rows[i].value);
    fprintf(stderr, "\n");
}

void recomp3ds::settings_menu_toggle() {
    g_open = !g_open;
    g_redraw = true;
}

void recomp3ds::settings_menu_redraw() { g_redraw = true; }

u32 recomp3ds::settings_menu_keys() {
    return g_open ? (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT) : 0;
}

// Called ~60 times a second from the main thread.
void recomp3ds::settings_menu_update() {
    static u32 prev = 0;
    static u64 next_repeat = 0;
    static int ticks = 0;
    // The game scans the pad only while it polls it (not during loads).
    hidScanInput();
    const u32 keys = hidKeysHeld() | autotest_keys();
    const u64 now = svcGetSystemTick();
    if (g_redraw) {
        draw();
        g_redraw = false;
    }
    else if (++ticks >= 60) {
        // The runtime's own messages land under the stats line.
        ticks = 0;
        blank_rows(kHintRow - 1, kHintRow - 1);
        draw_hint();
    }
    if (g_open) {
        const u32 pad = keys & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT);
        const u32 down = pad & ~prev;
        bool fire = false;
        u32 act = 0;
        if (down != 0) { fire = true; act = down; next_repeat = now + SYSCLOCK_ARM11 * 2 / 5; }
        else if (pad != 0 && now >= next_repeat) { fire = true; act = pad; next_repeat = now + SYSCLOCK_ARM11 / 12; }
        if (fire) {
            if (act & KEY_DUP) select_row(g_sel > 0 ? g_sel - 1 : g_row_count - 1);
            else if (act & KEY_DDOWN) select_row(g_sel + 1 < g_row_count ? g_sel + 1 : 0);
            else if (act & KEY_DLEFT) step(g_sel, -1);
            else if (act & KEY_DRIGHT) step(g_sel, +1);
        }

        // Touch: [-]/[+] on a row (held to repeat, choice rows only on the
        // press), or the row's label to choose it.
        static bool was_down = false;
        static u64 next_touch = 0;
        int tx = 0, ty = 0;
        bool touch = false;
        if (autotest_touch(&tx, &ty)) touch = true;
        else if (hidKeysHeld() & KEY_TOUCH) {
            touchPosition tp;
            hidTouchRead(&tp);
            tx = tp.px; ty = tp.py;
            touch = true;
        }
        bool press = touch && !was_down, tfire = false;
        if (press) { tfire = true; next_touch = now + SYSCLOCK_ARM11 * 2 / 5; }
        else if (touch && now >= next_touch) { tfire = true; next_touch = now + SYSCLOCK_ARM11 / 12; }
        was_down = touch;
        if (tfire) {
            const int col = tx / 8, line = ty / 8;
            for (int i = 0; i < g_row_count; i++) {
                if (line < row_line(i) - 1 || line > row_line(i) + 1) continue;
                int d = 0;
                if (col >= kMinusCol - 1 && col <= kMinusCol + 5) d = -1;
                else if (col >= kPlusCol - 1 && col <= kPlusCol + 5) d = 1;
                if (d != 0 && (press || g_rows[i].names == nullptr)) { select_row(i); step(i, d); }
                else if (press && col < kMinusCol - 1) select_row(i);
            }
        }
    }
    prev = keys;
    // Written a second after the last change, not on every step.
    if (g_dirty && now - g_changed_at > SYSCLOCK_ARM11) { save(); g_dirty = false; }
}
