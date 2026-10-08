// The settings menu on the touch screen. A tap of SELECT opens and closes it
// (holding SELECT takes a frame capture instead, see recomp3ds_run.cpp).
// While it is open the D-pad works it and the game does not see the D-pad:
// up/down chooses a row, left/right changes it (held to repeat); on the tab
// row at the top left/right changes the page. A tap chooses a tab or a row
// (values change with the D-pad only, which leaves the row's width to its
// label). Values go to <base>/settings.ini a second after the last change.
//
// The 3D page holds the stereo rows: the 3D slider sets the strength, these
// its shape (the owner's desktop settings, same units and defaults). The
// stick deadzones start the Controls page and C-Stick up follows the
// game's rows there; the game's own options (GameDesc::menu_options) go on
// the pages they name, and GameDesc::menu_defaults can change any row's
// default. A page with more rows than fit scrolls.
//
// With the runtime's mod system (RECOMP_STATIC_MODS), the Mods page lists
// the mods built into the executable once the game has started: an on/off
// row per mod (a baked code mod switches at once, nmr patch 0020), followed
// by the mod's options (applied at once). Their state is kept in
// settings.ini with everything else and handed to the runtime before the
// mods load (settings_apply_mods): the runtime's own mods.json and
// mod_config/ are never written on the 3DS (its writer thread is created
// before main and never runs). The game hears of a switch
// (GameDesc::on_mod_switched).
#include <3ds.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <deque>
#include <string>
#include <vector>

#include "rt64_3ds.h"
#include "recomp3ds.h"
#include "recomp3ds_internal.h"
#ifdef RECOMP_STATIC_MODS
#include "librecomp/mods.hpp"
#endif

namespace {

using recomp3ds::MenuPage;

char g_path[160] = "";
bool g_open = false, g_dirty = false, g_redraw = true;
u64 g_changed_at = 0;
int g_page = 0;
int g_sel = -1;                         // row of the page, -1 = the tab row
int g_top = 0;                          // the page's first visible row

// Console layout (40 x 30 characters): the game's name on row 0 and the
// stats on row 2 belong to recomp3ds_run.cpp, as do the HOME prompt (rows
// 23-26) and the capture message (row 28).
constexpr int kHintRow = 4, kTabRow = 6, kFirstRow = 8, kRowStep = 2, kLastRow = 22;
constexpr int kSlots = 7;               // rows 8..20 visible; row 22 is the page's note
// A page with more rows scrolls: markers on the note's line (only the 3D
// page has a note, and it does not scroll) show there are more above or
// below, and tapping one scrolls.
constexpr int kMoreRow = kLastRow, kMoreUpCol = 22, kMoreDownCol = 32;
// Touch: a target is its text line and this many pixels either side, so a
// touch between two neighbouring targets hits neither.
constexpr int kTouchPad = 3;
// Contact must be gone this many updates (~60 a second) to count as lifted:
// the panel drops it for a frame now and then, which read as a second tap.
constexpr int kLiftFrames = 4;
// A row: the selection marker, the label, the value. Values change with the
// D-pad; a tap on a row selects it.
constexpr int kLabelWidth = 25, kValueWidth = 12;

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
    // Rows kept elsewhere (the mods' state): not in settings.ini, and told
    // of changes with their context.
    bool no_save;
    void (*on_change_ctx)(void* ctx, int value);
    void* ctx;
};
constexpr int kMaxRows = 128;
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

// An Old 3DS's layout on a New 3DS (read at start; recomp3ds_run.cpp):
// core 2 left unused, the game on core 1 and the rest on core 0, or the
// other way round. Auto: three cores on a New 3DS, and on an Old 3DS the
// game's own choice (GameDesc::two_cores_game_on_core0).
const char* const kCoresNames[] = { "Auto", "2, game on 1", "2, game on 0" };
int g_cores = 0;
// settings.ini only: the touch screen's game-thread shares and audio rows.
int g_debug_rows = 0;
// settings.ini only: how much goes to log.txt (log.cpp), less on an Old 3DS.
int g_log_level = 2;
void apply_log_level(int v) { recomp3ds::log_set_level(v); }

// Audio on or off: off skips the audio microcode, which the CPU runs here,
// for speed (an Old 3DS).
int g_audio_on = 1;
const char* const kAudioNames[] = { "Off", "On" };
void apply_audio(int v) { recomp3ds::audio_set_enabled(v != 0); }
// Where the game's voices are made (GameDesc::synth): the DSP (dsp_synth.h),
// or with settings.ini's audio_engine=0 the audio task (its microcode,
// interpreted on the CPU). Not in the menu.
int g_audio_engine = 1;
const char* const kAudioEngineNames[] = { "RSP", "DSP" };
void apply_audio_engine(int v) { recomp3ds::dsp_synth_set_enabled(v == 1); }

// The frame cap (rt64_3ds::Settings::frame_cap): the game's own rate, or at
// most 30 drawn frames a second while it asks for more (the game and its
// audio keep their rate; every other frame stays on the screen twice), or,
// for games that offer it (RenderDesc::frame_interpolation), 60 while the
// game asks for 30: an interpolated frame between each two of the game's
// ("60 interpolated"; the row is 12 columns wide).
const char* const kFrameCapNames[] = { "Game", "30 fps", "60 interp." };

// Where the ROM lives, set in settings.ini only (it takes effect at start,
// so the menu does not show it): 0 auto (read from the SD card as needed on
// an Old 3DS, or when the heap is short; else in memory), 1 in memory, 2
// from the SD card. A row on no page: loaded and saved, never drawn.
int g_rom_stream = 0;

// Debugging, settings.ini only: 1 makes a SELECT hold save a frame capture
// for the host tools (gfx_task.bin, gfx_frame.ppm) instead of a screenshot.
int g_debug_capture = 0;

// Stick deadzones in percent of the travel.
int g_stick_dz = 5, g_cstick_dz = 20;
void apply_deadzones(int) { recomp3ds::input_set_deadzones(g_stick_dz, g_cstick_dz); }

// Whether pushing the C-Stick up presses C-Up (in many games first person,
// easy to hit while turning the camera).
int g_cstick_up = 1;
const char* const kCstickUpNames[] = { "Off", "C-Up" };
void apply_cstick_up(int v) { recomp3ds::input_set_cstick_up(v != 0); }

// GameDesc::cstick_camera_hold games: the C-Stick as the game's button
// camera (1, the default) or the plain C buttons (0).
int g_cstick_camera = 1;
const char* const kCstickCameraNames[] = { "C buttons", "Camera" };
const recomp3ds::GameDesc* g_cam_desc = nullptr;
void apply_cstick_camera(int v) {
    const recomp3ds::GameDesc* d = g_cam_desc;
    if (d == nullptr) return;
    recomp3ds::input_set_cstick_camera(v != 0 ? d->cstick_camera_hold : 0, d->cstick_camera_lead_ms, d->cstick_camera_press_ms,
                                       d->cstick_camera_period_ms);
}

void add_row(const Row& r) {
    if (g_row_count < kMaxRows) g_rows[g_row_count++] = r;
}

// The rows of a page, in order.
int page_rows(int page, int* out) {
    int n = 0;
    for (int i = 0; i < g_row_count; i++) {
        if (g_rows[i].page == page) out[n++] = i;
    }
    return n;
}
int page_count(int page) {
    int idx[kMaxRows];
    return page_rows(page, idx);
}


bool visible(int slot) { return slot >= g_top && slot < g_top + kSlots; }
int slot_line(int slot) { return kFirstRow + (slot - g_top) * kRowStep; }

void draw_row(int slot) {
    int idx[kMaxRows];
    if (slot < 0 || slot >= page_rows(g_page, idx) || !visible(slot)) return;
    const Row& r = g_rows[idx[slot]];
    char val[24];
    if (r.names != nullptr) snprintf(val, sizeof(val), "%s", r.names[*r.value - r.lo]);
    else if (*r.value == 0 && r.zero_text != nullptr) snprintf(val, sizeof(val), "%s", r.zero_text);
    else if (r.tenths) snprintf(val, sizeof(val), "%d.%d", *r.value / 10, *r.value % 10);
    else snprintf(val, sizeof(val), "%d%s", *r.value, r.suffix != nullptr ? r.suffix : "");
    const bool sel = slot == g_sel;
    recomp3ds::screen_printf("\x1b[%d;0H%s%s %-*.*s %-*.*s\x1b[0m", slot_line(slot), sel ? "\x1b[33m" : "", sel ? ">" : " ",
           kLabelWidth, kLabelWidth, r.label, kValueWidth, kValueWidth, val);
}

void draw_tabs() {
    // "[3D]  Controls  Camera  Game  Mods": the current page in brackets,
    // yellow while the tab row is chosen. Each tab is its name plus two
    // columns, one apart, so five fit the 40 columns.
    recomp3ds::screen_printf("\x1b[%d;0H%40s\x1b[%d;0H", kTabRow, "", kTabRow);
    int col = 0;
    for (int p = 0; p < recomp3ds::PageCount; p++) {
        g_tab_col[p] = -1;
        if (page_count(p) == 0) continue;
        if (col > 0) { recomp3ds::screen_printf(" "); col++; }
        g_tab_col[p] = col;
        const bool cur = p == g_page;
        if (cur && g_sel < 0) recomp3ds::screen_printf("\x1b[33m[%s]\x1b[0m", kPageNames[p]);
        else if (cur) recomp3ds::screen_printf("[%s]", kPageNames[p]);
        else recomp3ds::screen_printf(" %s ", kPageNames[p]);
        col += (int)strlen(kPageNames[p]) + 2;
    }
}

void blank_rows(int from, int to) {
    for (int r = from; r <= to; r++) recomp3ds::screen_printf("\x1b[%d;0H%40s", r, "");
}

void draw_hint() {
    recomp3ds::screen_printf("\x1b[%d;0H%-40s", kHintRow, g_open ? "SELECT: close   D-Pad: choose, change" : "SELECT: settings");
}

void draw_page() {
    blank_rows(kFirstRow - 1, kLastRow);
    const int n = page_count(g_page);
    for (int s = 0; s < n; s++) draw_row(s);
    if (g_top > 0) recomp3ds::screen_printf("\x1b[%d;%dH^ more", kMoreRow, kMoreUpCol);
    if (g_top + kSlots < n) recomp3ds::screen_printf("\x1b[%d;%dHv more", kMoreRow, kMoreDownCol);
    if (g_page == recomp3ds::Page3D) recomp3ds::screen_printf("\x1b[%d;0H3D slider: strength.  HUD 50: on screen.", kLastRow);
}

// Scrolls so that the first visible row is `top`.
void scroll_to(int top) {
    const int n = page_count(g_page);
    if (top > n - kSlots) top = n - kSlots;
    if (top < 0) top = 0;
    if (top == g_top) return;
    g_top = top;
    draw_page();
}

bool g_pad_redraw = true;              // the touch pad's rows were cleared

void draw() {
    g_pad_redraw = true;
    blank_rows(kHintRow - 1, kLastRow);
    draw_hint();
    if (!g_open) return;
    draw_tabs();
    draw_page();
}

// The built-in mods' state, also in settings.ini: "mod.<id>" on or off and
// "mod.<id>.<option>" the option's row value, whether or not a row shows it
// now (the Mods page is built once the game has started, and hides some
// mods in game modes); and "game_mode", the picker's last choice.
std::map<std::string, int> g_mod_values;
std::string g_game_mode;
LightLock g_file_lock;

void save() {
    LightLock_Lock(&g_file_lock);
    if (FILE* f = fopen(g_path, "w")) {
        for (int i = 0; i < g_row_count; i++) {
            if (!g_rows[i].no_save) fprintf(f, "%s=%d\n", g_rows[i].key, *g_rows[i].value);
        }
        for (const auto& [key, v] : g_mod_values) fprintf(f, "%s=%d\n", key.c_str(), v);
        if (!g_game_mode.empty()) fprintf(f, "game_mode=%s\n", g_game_mode.c_str());
        fclose(f);
    }
    LightLock_Unlock(&g_file_lock);
}

void load() {
    FILE* f = fopen(g_path, "r");
    if (f == nullptr) return;
    char line[192];
    while (fgets(line, sizeof(line), f)) {
        char key[128], text[64];
        int v;
        if (sscanf(line, "game_mode=%63s", text) == 1) { g_game_mode = text; continue; }
        if (sscanf(line, "%127[^=]=%d", key, &v) != 2) continue;
        if (strncmp(key, "mod.", 4) == 0) { g_mod_values[key] = v; continue; }
        for (int i = 0; i < g_row_count; i++) {
            const Row& r = g_rows[i];
            if (!r.no_save && strcmp(key, r.key) == 0) *r.value = v < r.lo ? r.lo : v > r.hi ? r.hi : v;
        }
    }
    fclose(f);
}

void changed() {
    g_dirty = true;
    g_changed_at = svcGetSystemTick();
}

void step(int slot, int d) {
    int idx[kMaxRows];
    if (slot < 0 || slot >= page_rows(g_page, idx)) return;
    Row& r = g_rows[idx[slot]];
    int v = *r.value + d * r.step;
    if (r.names != nullptr) v = v < r.lo ? r.hi : v > r.hi ? r.lo : v;
    else v = v < r.lo ? r.lo : v > r.hi ? r.hi : v;
    if (v == *r.value) return;
    *r.value = v;
    if (r.on_change != nullptr) r.on_change(v);
    if (r.on_change_ctx != nullptr) r.on_change_ctx(r.ctx, v);
    if (!r.no_save) changed();
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
    // Scroll the chosen row into view (redraws the page), else redraw the two.
    const int top = g_top;
    if (s >= 0 && s < g_top) scroll_to(s);
    else if (s >= g_top + kSlots) scroll_to(s - kSlots + 1);
    if (g_top != top) return;
    draw_row(old);
    draw_row(s);
}

void set_page(int p) {
    if (p == g_page || p < 0 || p >= recomp3ds::PageCount || page_count(p) == 0) return;
    g_page = p;
    g_top = 0;
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

// What a touch at (x, y) lands on.
enum TouchKind { TouchNone, TouchTab, TouchLabel, TouchUp, TouchDown };
struct TouchTarget {
    TouchKind kind = TouchNone;
    int arg = 0;                        // the page of a tab, the row of the others
    bool operator==(const TouchTarget& o) const { return kind == o.kind && arg == o.arg; }
};

TouchTarget touch_target(int x, int y) {
    const int col = x / 8;
    auto on_line = [&](int line) { return y >= line * 8 - kTouchPad && y < line * 8 + 8 + kTouchPad; };
    if (on_line(kTabRow)) {
        for (int p = 0; p < recomp3ds::PageCount; p++) {
            if (g_tab_col[p] >= 0 && col >= g_tab_col[p] && col < g_tab_col[p] + (int)strlen(kPageNames[p]) + 2) return { TouchTab, p };
        }
        return {};
    }
    if (on_line(kMoreRow)) {
        if (col >= kMoreDownCol - 1) return g_top + kSlots < page_count(g_page) ? TouchTarget{ TouchDown, 0 } : TouchTarget{};
        if (col >= kMoreUpCol - 1) return g_top > 0 ? TouchTarget{ TouchUp, 0 } : TouchTarget{};
        return {};
    }
    for (int v = 0; v < kSlots; v++) {
        if (!on_line(kFirstRow + v * kRowStep)) continue;
        const int s = g_top + v;
        if (s >= page_count(g_page)) return {};
        return { TouchLabel, s };
    }
    return {};
}

void touch_act(const TouchTarget& t) {
    switch (t.kind) {
        case TouchTab: set_page(t.arg); select_slot(-1); break;
        case TouchLabel: select_slot(t.arg); break;
        case TouchUp: scroll_to(g_top - 1); break;
        case TouchDown: scroll_to(g_top + 1); break;
        default: break;
    }
}

// Held, these repeat: the scroll markers.
bool touch_repeats(const TouchTarget& t) {
    return t.kind == TouchUp || t.kind == TouchDown;
}

// ---- The mods built into the executable (RECOMP_STATIC_MODS).

std::string g_mod_game_id;
void (*g_on_mod_switched)(const char* mod_id, bool on) = nullptr;
#ifdef RECOMP_STATIC_MODS
struct ModRow {
    std::string mod_id, option_id;      // option_id empty: the mod's on/off row
    std::string key;                    // in settings.ini (g_mod_values)
    std::string label;
    std::vector<std::string> names;
    std::vector<const char*> name_ptrs;
    std::vector<uint32_t> enum_values;  // an enum option's values, by row value
    enum Kind { Toggle, Enum, Bool, Number } kind = Toggle;
    double num_step = 1.0;
    int value = 0;
};
std::deque<ModRow> g_mod_rows;          // stable addresses: rows point into them
bool g_mods_added = false;
const char* const kModOffOn[] = { "Off", "On" };

// An option's row value as the mod's config value (enum rows count the
// options; number rows count steps).
recomp::config::ConfigValueVariant mod_option_value(const recomp::config::ConfigOption& o, int v) {
    switch (o.type) {
        case recomp::config::ConfigOptionType::Enum: {
            const auto& e = std::get<recomp::config::ConfigOptionEnum>(o.variant);
            return e.options[v < 0 ? 0 : (size_t)v < e.options.size() ? (size_t)v : e.options.size() - 1].value;
        }
        case recomp::config::ConfigOptionType::Bool: return v != 0;
        case recomp::config::ConfigOptionType::Number: {
            const auto& num = std::get<recomp::config::ConfigOptionNumber>(o.variant);
            return (double)v * (num.step > 0.0 ? num.step : 1.0);
        }
        default: return std::monostate{};
    }
}

void apply_mod_row(void* ctx, int v) {
    const ModRow& m = *static_cast<const ModRow*>(ctx);
    switch (m.kind) {
        case ModRow::Toggle:
            recomp::mods::enable_mod(m.mod_id, v != 0);
            if (g_on_mod_switched != nullptr) g_on_mod_switched(m.mod_id.c_str(), v != 0);
            break;
        case ModRow::Enum: recomp::mods::set_mod_config_value(m.mod_id, m.option_id, m.enum_values[v]); break;
        case ModRow::Bool: recomp::mods::set_mod_config_value(m.mod_id, m.option_id, v != 0); break;
        case ModRow::Number: recomp::mods::set_mod_config_value(m.mod_id, m.option_id, (double)v * m.num_step); break;
    }
    g_mod_values[m.key] = v;
    changed();
}

std::string clip(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(0, n); }

// The mods' rows, once the runtime has scanned them (after the game starts).
void add_mod_rows() {
    if (g_mods_added || g_mod_game_id.empty()) return;
    std::vector<recomp::mods::ModDetails> mods = recomp::mods::get_all_mod_details(g_mod_game_id);
    if (mods.empty()) return;
    g_mods_added = true;
    auto add = [](ModRow&& m, int lo, int hi, const char* const* names) {
        ModRow& row = g_mod_rows.emplace_back(std::move(m));
        if (!row.names.empty()) {
            for (const std::string& s : row.names) row.name_ptrs.push_back(s.c_str());
            names = row.name_ptrs.data();
        }
        add_row({ row.mod_id.c_str(), row.label.c_str(), lo, hi, &row.value, names, false, nullptr, nullptr, recomp3ds::PageMods, 1, nullptr,
                  true, apply_mod_row, &row });
    };
    for (const recomp::mods::ModDetails& d : mods) {
        if (d.custom_gamemode) continue;    // chosen at start, not toggled
        if (recomp3ds::mod_left_out(d.mod_id)) continue;   // not with this game mode
        ModRow toggle;
        toggle.mod_id = d.mod_id;
        toggle.key = "mod." + d.mod_id;
        toggle.label = clip(d.display_name, kLabelWidth);
        toggle.value = recomp::mods::is_mod_enabled(d.mod_id) ? 1 : 0;
        add(std::move(toggle), 0, 1, kModOffOn);
        const recomp::config::ConfigSchema& schema = recomp::mods::get_mod_config_schema(d.mod_id);
        for (const recomp::config::ConfigOption& o : schema.options) {
            if (o.hidden) continue;
            ModRow m;
            m.mod_id = d.mod_id;
            m.option_id = o.id;
            m.key = "mod." + d.mod_id + "." + o.id;
            m.label = " " + clip(o.name, kLabelWidth - 1);
            recomp::config::ConfigValueVariant cur = recomp::mods::get_mod_config_value(d.mod_id, o.id);
            if (o.type == recomp::config::ConfigOptionType::Enum) {
                const auto& e = std::get<recomp::config::ConfigOptionEnum>(o.variant);
                if (e.options.empty()) continue;
                m.kind = ModRow::Enum;
                for (size_t i = 0; i < e.options.size(); i++) {
                    m.names.push_back(clip(e.options[i].name.empty() ? e.options[i].key : e.options[i].name, kValueWidth));
                    m.enum_values.push_back(e.options[i].value);
                    if (std::holds_alternative<uint32_t>(cur) && std::get<uint32_t>(cur) == e.options[i].value) m.value = (int)i;
                }
                const int hi = (int)e.options.size() - 1;
                add(std::move(m), 0, hi, nullptr);
            }
            else if (o.type == recomp::config::ConfigOptionType::Bool) {
                m.kind = ModRow::Bool;
                m.value = std::holds_alternative<bool>(cur) && std::get<bool>(cur) ? 1 : 0;
                add(std::move(m), 0, 1, kModOffOn);
            }
            else if (o.type == recomp::config::ConfigOptionType::Number) {
                const auto& num = std::get<recomp::config::ConfigOptionNumber>(o.variant);
                m.kind = ModRow::Number;
                m.num_step = num.step > 0.0 ? num.step : 1.0;
                m.value = std::holds_alternative<double>(cur) ? (int)(std::get<double>(cur) / m.num_step + 0.5) : 0;
                add(std::move(m), (int)(num.min / m.num_step), (int)(num.max / m.num_step), nullptr);
            }
            // Strings keep their values: no text entry here.
        }
    }
}
#else
void add_mod_rows() {}
#endif

}   // namespace

#ifdef RECOMP_STATIC_MODS
// Before the mods load (the game's init, after the runtime's scan): every
// built-in code mod as settings.ini has it, off unless saved on.
void recomp3ds::settings_apply_mods() {
    for (const recomp::mods::ModDetails& d : recomp::mods::get_all_mod_details(g_mod_game_id)) {
        if (d.custom_gamemode) continue;
        const auto on = g_mod_values.find("mod." + d.mod_id);
        recomp::mods::enable_mod(d.mod_id, on != g_mod_values.end() && on->second != 0);
        const recomp::config::ConfigSchema& schema = recomp::mods::get_mod_config_schema(d.mod_id);
        for (const recomp::config::ConfigOption& o : schema.options) {
            const auto saved = g_mod_values.find("mod." + d.mod_id + "." + o.id);
            if (saved == g_mod_values.end()) continue;
            const recomp::config::ConfigValueVariant value = mod_option_value(o, saved->second);
            if (!std::holds_alternative<std::monostate>(value)) recomp::mods::set_mod_config_value(d.mod_id, o.id, value);
        }
    }
}
#else
void recomp3ds::settings_apply_mods() {}
#endif

std::string recomp3ds::settings_game_mode() { return g_game_mode; }

void recomp3ds::settings_set_game_mode(const std::string& mod_id) {
    if (mod_id == g_game_mode) return;
    g_game_mode = mod_id;
    save();
}

void recomp3ds::settings_menu_init(const char* base_path, const GameDesc& desc) {
    LightLock_Init(&g_file_lock);
    snprintf(g_path, sizeof(g_path), "%s/settings.ini", base_path);
    rt64_3ds::Settings& st = rt64_3ds::settings();
    g_conv_tenths = st.convergence_hundredths / 10;
    g_auto = st.auto_convergence ? 1 : 0;
    g_row_count = 0;
    g_mod_game_id = desc.entry.mod_game_id;
    g_on_mod_switched = desc.on_mod_switched;
    add_row({ "separation", "3D depth", 0, 100, &st.sep_slider, nullptr, false, nullptr, nullptr, Page3D, 1, nullptr });
    add_row({ "convergence_tenths", "Convergence", 1, 200, &g_conv_tenths, nullptr, true, nullptr, apply_conv, Page3D, 1, nullptr });
    add_row({ "hud_depth", "HUD depth", 0, 100, &st.hud_depth, nullptr, false, nullptr, nullptr, Page3D, 1, nullptr });
    add_row({ "auto_convergence", "Auto convergence", 0, 1, &g_auto, kOffOn, false, nullptr, apply_auto, Page3D, 1, nullptr });
    add_row({ "comfort_target", "Comfort target", -20, 30, &st.comfort_target, nullptr, false, nullptr, nullptr, Page3D, 1, nullptr });
    // The desktop's ghost reduction: 100% contrast and a 0% floor are off.
    add_row({ "stereo_ghost_contrast", "Ghost reduction: contrast", 50, 100, &st.ghost_contrast, nullptr, false, "%", nullptr, Page3D, 1, nullptr });
    add_row({ "stereo_ghost_black_floor", "Ghost black floor", 0, 20, &st.ghost_black_floor, nullptr, false, "%", nullptr, Page3D, 1, nullptr });
    add_row({ "stick_deadzone", "Circle Pad deadzone", 0, 50, &g_stick_dz, nullptr, false, "%", apply_deadzones, PageControls, 1, nullptr });
    add_row({ "cstick_deadzone", "C-Stick deadzone", 0, 50, &g_cstick_dz, nullptr, false, "%", apply_deadzones, PageControls, 1, nullptr });
    for (size_t i = 0; i < desc.menu_option_count; i++) {
        const MenuOption& o = desc.menu_options[i];
        add_row({ o.key, o.label, o.lo, o.hi, o.value, o.names, false, o.suffix, o.on_change, o.page, o.step > 0 ? o.step : 1, o.zero_text });
    }
    // After the game's rows: where C-Up is a camera (DK64's first person) it
    // follows the game's camera settings.
    if (desc.cstick_camera_hold != 0) {
        g_cam_desc = &desc;
        add_row({ "cstick_camera", "C-Stick", 0, 1, &g_cstick_camera, kCstickCameraNames, false, nullptr, apply_cstick_camera, PageControls, 1, nullptr });
    }
    add_row({ "cstick_up", "C-Stick up", 0, 1, &g_cstick_up, kCstickUpNames, false, nullptr, apply_cstick_up, PageControls, 1, nullptr });
    add_row({ "audio", "Audio", 0, 1, &g_audio_on, kAudioNames, false, nullptr, apply_audio, PageGame, 1, nullptr });
    add_row({ "frame_cap", "Frame rate", 0, desc.render.frame_interpolation ? 2 : 1, &st.frame_cap, kFrameCapNames, false, nullptr, nullptr, PageGame, 1, nullptr });
    add_row({ "rom_stream", "ROM stream", 0, 2, &g_rom_stream, nullptr, false, nullptr, nullptr, recomp3ds::PageCount, 1, nullptr });
    add_row({ "debug_capture", "Debug capture", 0, 1, &g_debug_capture, nullptr, false, nullptr, nullptr, recomp3ds::PageCount, 1, nullptr });
    // The renderer's GPU-cost choices (RenderDesc::eye_color16 and so on),
    // settings.ini only and read at start: the game's choice unless the file
    // says otherwise, for A/B tests on the console.
    st.gpu_color16 = desc.render.eye_color16 ? 1 : 0;
    st.gpu_depth16 = desc.render.eye_depth16 ? 1 : 0;
    st.gpu_textures16 = desc.render.intensity_textures16 ? 1 : 0;
    st.gpu_fill_clear = desc.render.fill_clear ? 1 : 0;
    st.gpu_double_eyes = desc.render.double_eyes ? 1 : 0;
    add_row({ "gpu_color16", "GPU 16-bit colour", 0, 1, &st.gpu_color16, nullptr, false, nullptr, nullptr, recomp3ds::PageCount, 1, nullptr });
    add_row({ "gpu_depth16", "GPU 16-bit depth", 0, 1, &st.gpu_depth16, nullptr, false, nullptr, nullptr, recomp3ds::PageCount, 1, nullptr });
    add_row({ "gpu_textures16", "GPU 16-bit I/IA textures", 0, 1, &st.gpu_textures16, nullptr, false, nullptr, nullptr, recomp3ds::PageCount, 1, nullptr });
    add_row({ "gpu_fill_clear", "GPU fill clears", 0, 1, &st.gpu_fill_clear, nullptr, false, nullptr, nullptr, recomp3ds::PageCount, 1, nullptr });
    add_row({ "gpu_double_eyes", "GPU double eye targets", 0, 1, &st.gpu_double_eyes, nullptr, false, nullptr, nullptr, recomp3ds::PageCount, 1, nullptr });
    bool is_new_3ds = false;
    APT_CheckNew3DS(&is_new_3ds);
    if (is_new_3ds) add_row({ "cpu_speed", "CPU speed", 0, 1, &g_cpu_new, kCpuNames, false, nullptr, apply_cpu, PageGame, 1, nullptr });
    add_row({ "cores", "Cores (at restart)", 0, 2, &g_cores, kCoresNames, false, nullptr, nullptr, PageGame, 1, nullptr });
    add_row({ "debug_rows", "Debug rows", 0, 1, &g_debug_rows, nullptr, false, nullptr, nullptr, recomp3ds::PageCount, 1, nullptr });
    g_log_level = is_new_3ds ? 2 : 0;
    add_row({ "log_level", "Log level", 0, 2, &g_log_level, nullptr, false, nullptr, apply_log_level, recomp3ds::PageCount, 1, nullptr });
    if (desc.synth.flavor != recomp3ds::SynthFlavor::None) {
        // settings.ini only (audio_engine=0 runs the audio task as before).
        add_row({ "audio_engine", "Audio engine", 0, 1, &g_audio_engine, kAudioEngineNames, false, nullptr, apply_audio_engine, recomp3ds::PageCount, 1, nullptr });
    }
    // The game's defaults, then what settings.ini holds.
    for (size_t d = 0; d < desc.menu_default_count; d++) {
        const MenuDefault& md = desc.menu_defaults[d];
        for (int i = 0; i < g_row_count; i++) {
            const Row& r = g_rows[i];
            if (strcmp(md.key, r.key) == 0) *r.value = md.value < r.lo ? r.lo : md.value > r.hi ? r.hi : md.value;
        }
    }
    load();
    for (int i = 0; i < g_row_count; i++) {
        if (g_rows[i].on_change != nullptr) g_rows[i].on_change(*g_rows[i].value);
    }
    fprintf(stderr, "recomp3ds: settings:");
    for (int i = 0; i < g_row_count; i++) fprintf(stderr, " %s %d", g_rows[i].key, *g_rows[i].value);
    fprintf(stderr, "\n");
}

int recomp3ds::settings_rom_stream() { return g_rom_stream; }
int recomp3ds::settings_cores() { return g_cores; }
bool recomp3ds::settings_debug_rows() { return g_debug_rows != 0; }
bool recomp3ds::settings_debug_capture() { return g_debug_capture != 0; }

void recomp3ds::settings_menu_toggle() {
    g_open = !g_open;
    if (g_open) add_mod_rows();
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

        // Touch: a tab, a row to choose it, or a scroll marker. A touch acts once its position has settled (its
        // second update), on what it landed on, and after that only on that
        // same target until it lifts: held on a repeating one, and while
        // still on it.
        static int contact_frames = 0, lift_frames = kLiftFrames;
        static TouchTarget locked;
        static bool acted = false;
        static u64 next_touch = 0;
        int tx = 0, ty = 0;
        bool contact = false;
        if (autotest_touch(&tx, &ty)) contact = true;
        else if (hidKeysHeld() & KEY_TOUCH) {
            touchPosition tp;
            hidTouchRead(&tp);
            tx = tp.px; ty = tp.py;
            contact = true;
        }
        if (contact) { lift_frames = 0; contact_frames++; }
        else if (lift_frames < kLiftFrames) lift_frames++;
        if (!contact && lift_frames >= kLiftFrames) {
            contact_frames = 0;
            acted = false;
            locked = {};
        }
        else if (contact && !acted && contact_frames >= 2) {
            locked = touch_target(tx, ty);
            touch_act(locked);
            acted = true;
            next_touch = now + SYSCLOCK_ARM11 / 2;
        }
        else if (contact && acted && now >= next_touch && touch_repeats(locked)) {
            if (touch_target(tx, ty) == locked) touch_act(locked);
            next_touch = now + SYSCLOCK_ARM11 / 12;
        }
    }
    else {
        // The touch pad's buttons while the menu is closed.
        int tx = 0, ty = 0;
        bool contact = autotest_touch(&tx, &ty);
        if (!contact && (hidKeysHeld() & KEY_TOUCH)) {
            touchPosition tp;
            hidTouchRead(&tp);
            tx = tp.px; ty = tp.py;
            contact = true;
        }
        touch_pad_update(true, contact, tx, ty, g_pad_redraw);
        g_pad_redraw = false;
    }
    if (g_open) touch_pad_update(false, false, 0, 0, false);
    prev = keys;
    // Written a second after the last change, not on every step.
    if (g_dirty && now - g_changed_at > SYSCLOCK_ARM11) { save(); g_dirty = false; }
}
