// The settings menu on the touch screen. A tap of SELECT opens and closes it
// (holding SELECT takes a frame capture instead, see recomp3ds_run.cpp).
// While it is open the D-pad works it and the game does not see the D-pad:
// up/down chooses a row, left/right changes it (held to repeat); on the tab
// row at the top left/right changes the page. Tabs, rows and the [-]/[+]
// buttons work by touch as well. Values go to <base>/settings.ini a second
// after the last change.
//
// The 3D page holds the stereo rows: the 3D slider sets the strength, these
// its shape (the owner's desktop settings, same units and defaults). The
// stick deadzones start the Controls page and C-Stick up ends the Camera
// page; the game's own options (GameDesc::menu_options) go on the pages
// they name.
#include <3ds.h>
#include <cstdio>
#include <cstring>

#include "rt64_3ds.h"
#include "recomp3ds.h"
#include "recomp3ds_internal.h"

namespace {

using recomp3ds::MenuPage;

char g_path[160] = "";
bool g_open = false, g_dirty = false, g_redraw = true;
u64 g_changed_at = 0;
int g_page = 0;
int g_sel = -1;                         // slot on the page, -1 = the tab row

// Console layout (40 x 30 characters): the game's name on row 0 and the
// stats on row 2 belong to recomp3ds_run.cpp, as do the HOME prompt (rows
// 23-26) and the capture message (row 28).
constexpr int kHintRow = 4, kTabRow = 6, kFirstRow = 8, kRowStep = 2, kLastRow = 22;
constexpr int kSlots = 7;               // rows 8..20; row 22 is the page's note
constexpr int kMinusCol = 16, kValueWidth = 11, kPlusCol = 35;

const char* const kPageNames[recomp3ds::PageCount] = { "3D", "Controls", "Camera", "Game", "Mods" };
int g_tab_col[recomp3ds::PageCount];

struct Row {
    const char* key;
    const char* label;
    int lo, hi;
    int* value;
    const char* const* names;       // shown instead of the number; stepping wraps
    bool tenths;                    // shown as n.n
    const char* suffix;             // after the number ("%")
    void (*on_change)(int value);
    int page, step;
    const char* zero_text;          // shown for 0 ("Off")
};
constexpr int kMaxRows = 32;
Row g_rows[kMaxRows];
int g_row_count = 0;

// Convergence is kept in hundredths (0.2 world units each) but shown and
// stepped in tenths like the desktop slider; auto-convergence is a bool.
int g_conv_tenths = 60;
int g_auto = 1;
const char* const kOffOn[] = { "off", "on" };

void apply_conv(int v) { rt64_3ds::settings().convergence_hundredths = v * 10; }
void apply_auto(int v) { rt64_3ds::settings().auto_convergence = v != 0; }

// The New 3DS CPU speed, to see how an Old 3DS would fare.
int g_cpu_new = 1;
const char* const kCpuNames[] = { "Old 3DS", "New 3DS" };
void apply_cpu(int v) { recomp3ds::set_cpu_speed(v != 0); }

// Stick deadzones in percent of the travel.
int g_stick_dz = 5, g_cstick_dz = 20;
void apply_deadzones(int) { recomp3ds::input_set_deadzones(g_stick_dz, g_cstick_dz); }

// Whether pushing the C-Stick up presses C-Up (in many games first person,
// easy to hit while turning the camera).
int g_cstick_up = 1;
const char* const kCstickUpNames[] = { "Off", "C-Up" };
void apply_cstick_up(int v) { recomp3ds::input_set_cstick_up(v != 0); }

void add_row(const Row& r) {
    if (g_row_count < kMaxRows) g_rows[g_row_count++] = r;
}

// The rows of the current page, in order.
int page_rows(int page, int* out) {
    int n = 0;
    for (int i = 0; i < g_row_count && n < kSlots; i++) {
        if (g_rows[i].page == page) out[n++] = i;
    }
    return n;
}
int page_count(int page) {
    int idx[kSlots];
    return page_rows(page, idx);
}

int slot_line(int slot) { return kFirstRow + slot * kRowStep; }

void draw_row(int slot) {
    int idx[kSlots];
    if (slot < 0 || slot >= page_rows(g_page, idx)) return;
    const Row& r = g_rows[idx[slot]];
    char val[24];
    if (r.names != nullptr) snprintf(val, sizeof(val), "%s", r.names[*r.value - r.lo]);
    else if (*r.value == 0 && r.zero_text != nullptr) snprintf(val, sizeof(val), "%s", r.zero_text);
    else if (r.tenths) snprintf(val, sizeof(val), "%d.%d", *r.value / 10, *r.value % 10);
    else snprintf(val, sizeof(val), "%d%s", *r.value, r.suffix != nullptr ? r.suffix : "");
    // Centred in its field.
    const int len = (int)strlen(val) < kValueWidth ? (int)strlen(val) : kValueWidth;
    const int left = (kValueWidth - len) / 2;
    const bool sel = slot == g_sel;
    printf("\x1b[%d;0H%s%s %-13s\x1b[0m [ - ] %*s%-*s  [ + ]", slot_line(slot), sel ? "\x1b[33m" : "", sel ? ">" : " ", r.label,
           left + len, val, kValueWidth - left - len, "");
}

void draw_tabs() {
    // "[3D]  Controls  Camera  Game  Mods": the current page in brackets,
    // yellow while the tab row is chosen. Each tab is its name plus two
    // columns, one apart, so five fit the 40 columns.
    printf("\x1b[%d;0H%40s\x1b[%d;0H", kTabRow, "", kTabRow);
    int col = 0;
    for (int p = 0; p < recomp3ds::PageCount; p++) {
        g_tab_col[p] = -1;
        if (page_count(p) == 0) continue;
        if (col > 0) { printf(" "); col++; }
        g_tab_col[p] = col;
        const bool cur = p == g_page;
        if (cur && g_sel < 0) printf("\x1b[33m[%s]\x1b[0m", kPageNames[p]);
        else if (cur) printf("[%s]", kPageNames[p]);
        else printf(" %s ", kPageNames[p]);
        col += (int)strlen(kPageNames[p]) + 2;
    }
}

void blank_rows(int from, int to) {
    for (int r = from; r <= to; r++) printf("\x1b[%d;0H%40s", r, "");
}

void draw_hint() {
    printf("\x1b[%d;0H%-40s", kHintRow, g_open ? "SELECT: close   D-Pad: choose, change" : "SELECT: settings");
}

void draw_page() {
    blank_rows(kFirstRow - 1, kLastRow);
    const int n = page_count(g_page);
    for (int s = 0; s < n; s++) draw_row(s);
    if (g_page == recomp3ds::Page3D) printf("\x1b[%d;0H3D slider: strength.  HUD 50: on screen.", kLastRow);
}

void draw() {
    blank_rows(kHintRow - 1, kLastRow);
    draw_hint();
    if (!g_open) return;
    draw_tabs();
    draw_page();
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

void step(int slot, int d) {
    int idx[kSlots];
    if (slot < 0 || slot >= page_rows(g_page, idx)) return;
    Row& r = g_rows[idx[slot]];
    int v = *r.value + d * r.step;
    if (r.names != nullptr) v = v < r.lo ? r.hi : v > r.hi ? r.lo : v;
    else v = v < r.lo ? r.lo : v > r.hi ? r.hi : v;
    if (v == *r.value) return;
    *r.value = v;
    if (r.on_change != nullptr) r.on_change(v);
    changed();
    draw_row(slot);
}

void select_slot(int s) {
    const int n = page_count(g_page);
    if (s >= n) s = n - 1;
    if (s < -1) s = -1;
    if (s == g_sel) return;
    const int old = g_sel;
    g_sel = s;
    if (old < 0 || s < 0) draw_tabs();
    draw_row(old);
    draw_row(s);
}

void set_page(int p) {
    if (p == g_page || p < 0 || p >= recomp3ds::PageCount || page_count(p) == 0) return;
    g_page = p;
    draw_tabs();
    draw_page();
}

// The next non-empty page that way, wrapping.
void turn_page(int d) {
    for (int k = 1; k < recomp3ds::PageCount; k++) {
        const int p = (g_page + d * k + recomp3ds::PageCount * 2) % recomp3ds::PageCount;
        if (page_count(p) != 0) { set_page(p); return; }
    }
}

}   // namespace

void recomp3ds::settings_menu_init(const char* base_path, const GameDesc& desc) {
    snprintf(g_path, sizeof(g_path), "%s/settings.ini", base_path);
    rt64_3ds::Settings& st = rt64_3ds::settings();
    g_conv_tenths = st.convergence_hundredths / 10;
    g_auto = st.auto_convergence ? 1 : 0;
    g_row_count = 0;
    add_row({ "separation", "3D depth", 0, 100, &st.sep_slider, nullptr, false, nullptr, nullptr, Page3D, 1, nullptr });
    add_row({ "convergence_tenths", "Convergence", 1, 200, &g_conv_tenths, nullptr, true, nullptr, apply_conv, Page3D, 1, nullptr });
    add_row({ "hud_depth", "HUD depth", 0, 100, &st.hud_depth, nullptr, false, nullptr, nullptr, Page3D, 1, nullptr });
    add_row({ "auto_convergence", "Auto conv.", 0, 1, &g_auto, kOffOn, false, nullptr, apply_auto, Page3D, 1, nullptr });
    add_row({ "comfort_target", "Comfort", -20, 30, &st.comfort_target, nullptr, false, nullptr, nullptr, Page3D, 1, nullptr });
    // The desktop's ghost reduction: 100% contrast and a 0% floor are off.
    add_row({ "stereo_ghost_contrast", "Ghost contr.", 50, 100, &st.ghost_contrast, nullptr, false, "%", nullptr, Page3D, 1, nullptr });
    add_row({ "stereo_ghost_black_floor", "Black floor", 0, 20, &st.ghost_black_floor, nullptr, false, "%", nullptr, Page3D, 1, nullptr });
    add_row({ "stick_deadzone", "Circle Pad dz", 0, 50, &g_stick_dz, nullptr, false, "%", apply_deadzones, PageControls, 1, nullptr });
    add_row({ "cstick_deadzone", "C-Stick dz", 0, 50, &g_cstick_dz, nullptr, false, "%", apply_deadzones, PageControls, 1, nullptr });
    for (size_t i = 0; i < desc.menu_option_count; i++) {
        const MenuOption& o = desc.menu_options[i];
        add_row({ o.key, o.label, o.lo, o.hi, o.value, o.names, false, o.suffix, o.on_change, o.page, o.step > 0 ? o.step : 1, o.zero_text });
    }
    // After the game's camera rows: in a game where C-Up is a camera
    // (DK64's first person), this is a camera setting.
    add_row({ "cstick_up", "C-Stick up", 0, 1, &g_cstick_up, kCstickUpNames, false, nullptr, apply_cstick_up, PageCamera, 1, nullptr });
    bool is_new_3ds = false;
    APT_CheckNew3DS(&is_new_3ds);
    if (is_new_3ds) add_row({ "cpu_speed", "CPU speed", 0, 1, &g_cpu_new, kCpuNames, false, nullptr, apply_cpu, PageGame, 1, nullptr });
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
            const int n = page_count(g_page);
            if (act & KEY_DUP) select_slot(g_sel >= 0 ? g_sel - 1 : n - 1);
            else if (act & KEY_DDOWN) select_slot(g_sel + 1 < n ? g_sel + 1 : -1);
            else if (act & (KEY_DLEFT | KEY_DRIGHT)) {
                const int d = (act & KEY_DLEFT) ? -1 : 1;
                // Pages turn on the press only.
                if (g_sel < 0) { if (down & (KEY_DLEFT | KEY_DRIGHT)) turn_page(d); }
                else step(g_sel, d);
            }
        }

        // Touch: a tab, [-]/[+] on a row (held to repeat, choice rows only
        // on the press), or the row's label to choose it.
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
            if (press && line >= kTabRow - 1 && line <= kTabRow) {
                for (int p = recomp3ds::PageCount - 1; p >= 0; p--) {
                    if (g_tab_col[p] >= 0 && col >= g_tab_col[p]) { set_page(p); select_slot(-1); break; }
                }
            }
            else {
                // Each row owns its line and the one above it.
                const int s = line >= kFirstRow - 1 ? (line - kFirstRow + 1) / kRowStep : -1;
                const Row* r = nullptr;
                int idx[kSlots];
                if (s >= 0 && s < page_rows(g_page, idx)) r = &g_rows[idx[s]];
                if (r != nullptr) {
                    int d = 0;
                    if (col >= kMinusCol - 1 && col <= kMinusCol + 5) d = -1;
                    else if (col >= kPlusCol - 1 && col <= kPlusCol + 5) d = 1;
                    if (d != 0 && (press || r->names == nullptr)) { select_slot(s); step(s, d); }
                    else if (press && col < kMinusCol - 1) select_slot(s);
                }
            }
        }
    }
    prev = keys;
    // Written a second after the last change, not on every step.
    if (g_dirty && now - g_changed_at > SYSCLOCK_ARM11) { save(); g_dirty = false; }
}
