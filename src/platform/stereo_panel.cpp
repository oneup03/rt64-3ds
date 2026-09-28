// The stereo settings on the touch screen: text rows on the bottom console
// with [-]/[+] buttons (hold to repeat), saved to <base>/settings.ini. The
// 3D slider sets the strength; these set its shape (the owner's desktop
// settings, same units and defaults).
#include <3ds.h>
#include <cstdio>
#include <cstring>

#include "rt64_3ds.h"
#include "recomp3ds_internal.h"

namespace {

char g_path[160] = "";
bool g_dirty = false, g_redraw = true;
u64 g_changed_at = 0;

constexpr int kFirstRow = 8;

// One adjustable row: its console row and how it reads and steps.
struct Row {
    const char* label;
    int row;
    int lo, hi, step;
    int* value;
};

// Convergence is kept in hundredths (0.2 world units each) but shown and
// stepped in tenths like the desktop slider.
int g_conv_tenths = 60;

Row g_rows[] = {
    { "3D depth",    kFirstRow + 2, 0, 50, 1, nullptr },
    { "Convergence", kFirstRow + 4, 1, 200, 1, nullptr },
    { "HUD depth",   kFirstRow + 6, 0, 100, 1, nullptr },
    { "Comfort",     kFirstRow + 10, -20, 30, 1, nullptr },
};
constexpr int kAutoRow = kFirstRow + 8;
constexpr int kMinusCol = 14, kPlusCol = 27;   // "[ - ]" and "[ + ]" start columns

void bind_rows() {
    rt64_3ds::Settings& st = rt64_3ds::settings();
    g_rows[0].value = &st.sep_slider;
    g_rows[1].value = &g_conv_tenths;
    g_rows[2].value = &st.hud_depth;
    g_rows[3].value = &st.comfort_target;
}

void apply_conv() { rt64_3ds::settings().convergence_hundredths = g_conv_tenths * 10; }

void draw() {
    rt64_3ds::Settings& st = rt64_3ds::settings();
    printf("\x1b[%d;0H----------------- 3D -----------------", kFirstRow);
    for (const Row& r : g_rows) {
        char val[16];
        if (r.value == &g_conv_tenths) snprintf(val, sizeof(val), "%d.%d", *r.value / 10, *r.value % 10);
        else snprintf(val, sizeof(val), "%d", *r.value);
        printf("\x1b[%d;0H%-12s  [ - ] %6s  [ + ]  ", r.row, r.label, val);
    }
    printf("\x1b[%d;0H%-12s  [ %s ]              ", kAutoRow, "Auto conv.", st.auto_convergence ? "on " : "off");
    printf("\x1b[%d;0HThe 3D slider sets the strength.", kFirstRow + 12);
    printf("\x1b[%d;0HHUD depth 50 = on the screen.   ", kFirstRow + 13);
}

void save() {
    FILE* f = fopen(g_path, "w");
    if (f == nullptr) return;
    const rt64_3ds::Settings& st = rt64_3ds::settings();
    fprintf(f, "separation=%d\nconvergence_tenths=%d\nhud_depth=%d\nauto_convergence=%d\ncomfort_target=%d\n",
            st.sep_slider, g_conv_tenths, st.hud_depth, st.auto_convergence ? 1 : 0, st.comfort_target);
    fclose(f);
}

void load() {
    FILE* f = fopen(g_path, "r");
    if (f == nullptr) return;
    rt64_3ds::Settings& st = rt64_3ds::settings();
    char line[96];
    while (fgets(line, sizeof(line), f)) {
        char key[48];
        int v;
        if (sscanf(line, "%47[^=]=%d", key, &v) != 2) continue;
        if (strcmp(key, "separation") == 0) st.sep_slider = v < 0 ? 0 : v > 50 ? 50 : v;
        else if (strcmp(key, "convergence_tenths") == 0) g_conv_tenths = v < 1 ? 1 : v > 200 ? 200 : v;
        else if (strcmp(key, "hud_depth") == 0) st.hud_depth = v < 0 ? 0 : v > 100 ? 100 : v;
        else if (strcmp(key, "auto_convergence") == 0) st.auto_convergence = v != 0;
        else if (strcmp(key, "comfort_target") == 0) st.comfort_target = v < -20 ? -20 : v > 30 ? 30 : v;
    }
    fclose(f);
}

void changed() {
    g_dirty = true;
    g_redraw = true;
    g_changed_at = svcGetSystemTick();
}

}   // namespace

void recomp3ds::stereo_panel_init(const char* base_path) {
    snprintf(g_path, sizeof(g_path), "%s/settings.ini", base_path);
    g_conv_tenths = rt64_3ds::settings().convergence_hundredths / 10;
    load();
    bind_rows();
    apply_conv();
    const rt64_3ds::Settings& st = rt64_3ds::settings();
    fprintf(stderr, "recomp3ds: stereo settings: separation %d convergence %d.%d hud %d auto %d comfort %d\n",
            st.sep_slider, g_conv_tenths / 10, g_conv_tenths % 10, st.hud_depth, (int)st.auto_convergence, st.comfort_target);
}

// Called ~60 times a second from the main thread.
void recomp3ds::stereo_panel_update() {
    static bool was_down = false;
    static u64 next_repeat = 0;
    int tx = 0, ty = 0;
    bool down = false;
    if (autotest_touch(&tx, &ty)) {
        down = true;
    }
    else if (hidKeysHeld() & KEY_TOUCH) {
        touchPosition tp;
        hidTouchRead(&tp);
        tx = tp.px; ty = tp.py;
        down = true;
    }
    const u64 now = svcGetSystemTick();
    bool fire = false;
    if (down && !was_down) { fire = true; next_repeat = now + SYSCLOCK_ARM11 * 2 / 5; }
    else if (down && now >= next_repeat) { fire = true; next_repeat = now + SYSCLOCK_ARM11 / 12; }
    was_down = down;
    if (fire) {
        const int col = tx / 8, row = ty / 8;
        for (Row& r : g_rows) {
            if (row < r.row - 1 || row > r.row + 1) continue;
            int d = 0;
            if (col >= kMinusCol - 1 && col <= kMinusCol + 5) d = -r.step;
            else if (col >= kPlusCol - 1 && col <= kPlusCol + 5) d = r.step;
            if (d != 0) {
                int v = *r.value + d;
                v = v < r.lo ? r.lo : v > r.hi ? r.hi : v;
                if (v != *r.value) { *r.value = v; apply_conv(); changed(); }
            }
        }
        if (row >= kAutoRow - 1 && row <= kAutoRow + 1 && col >= kMinusCol - 1 && col <= kMinusCol + 8) {
            // Toggles only on the press, not on repeat.
            static u64 last_toggle = 0;
            if (now - last_toggle > SYSCLOCK_ARM11 / 3) {
                last_toggle = now;
                rt64_3ds::settings().auto_convergence = !rt64_3ds::settings().auto_convergence;
                changed();
            }
        }
    }
    if (g_redraw) { draw(); g_redraw = false; }
    // Written a second after the last change, not on every step.
    if (g_dirty && now - g_changed_at > SYSCLOCK_ARM11) { save(); g_dirty = false; }
}
