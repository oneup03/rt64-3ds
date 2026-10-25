// ultramodern input callbacks on libctru's HID: one N64 controller from the
// 3DS pad. The buttons follow the game's ButtonMap (GameDesc), or by default:
//   A=A  B=B  Y=Z  ZL=Z  X=C-up  L=L  R/ZR=R  D-pad=D-pad  START=START
// and the Controls page maps each of A, B, X, Y, L, R, ZL, ZR and the
// D-pad's directions to an N64 button of the player's choice (defaults:
// the game's map). The Circle Pad is the stick and the C-Stick (New 3DS)
// the C buttons. SELECT belongs to the platform (settings menu, frame
// capture), and the D-pad to the menu while it is open.
#include <3ds.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ultramodern/input.hpp"
#include "recomp3ds_internal.h"
#include "recomp3ds.h"
#include "GamepadMotion.hpp"

namespace {
using namespace recomp3ds;
constexpr int CPAD_MAX = 156;
// A C-Stick direction presses its C button past a quarter of the range left
// after the deadzone (at the default 20% that is ~40% of the raw travel).
constexpr float CSTICK_BUTTON = 0.25f;
u32 g_held = 0;
circlePosition g_cpad{};
circlePosition g_cstick{};
volatile bool g_blocked = false;
volatile bool g_cstick_buttons = true;
volatile bool g_cstick_up = true;       // C-Stick up presses C-Up (settings menu)
volatile bool g_cstick_down = true;     // C-Stick down presses C-Down (settings menu)
volatile float g_stick_dz = 0.05f, g_cstick_dz = 0.20f;

// GameDesc::cstick_camera_*: the C-Stick holds these buttons and repeats a C
// press while it points a way (0: the plain C buttons). The pulse state lives
// on the game thread (input_get).
struct CstickCamera { uint16_t hold, lead_ms, press_ms, period_ms; };
std::atomic<uint64_t> g_cam_cfg{ 0 };
uint16_t g_cam_dir = 0;          // the C button being pulsed (0: stick centred)
uint64_t g_cam_held_since = 0;   // ms: when the hold buttons went down
uint64_t g_cam_dir_since = 0;    // ms: when this direction's presses start

CstickCamera cam_cfg() {
    const uint64_t v = g_cam_cfg.load();
    return CstickCamera{ uint16_t(v), uint16_t(v >> 16), uint16_t(v >> 32), uint16_t(v >> 48) };
}

// The buttons the C-Stick presses in camera mode: the hold buttons while it
// points a way, and that way's C button for press_ms of every period_ms,
// starting lead_ms after the hold buttons (the game must see those first:
// Goemon ignores a C press that comes with R in the same frame).
uint16_t cstick_camera(const CstickCamera& c, float cx, float cy) {
    uint16_t dir = 0;
    if (fabsf(cx) >= fabsf(cy)) {
        if (cx < -CSTICK_BUTTON) dir = N64_CLEFT;
        else if (cx > CSTICK_BUTTON) dir = N64_CRIGHT;
    }
    else {
        if (cy > CSTICK_BUTTON && g_cstick_up) dir = N64_CUP;
        else if (cy < -CSTICK_BUTTON && g_cstick_down) dir = N64_CDOWN;
    }
    if (dir == 0) { g_cam_dir = 0; return 0; }
    const uint64_t now = recomp3ds::wall_ms();
    if (g_cam_dir == 0) {
        g_cam_held_since = now;
        g_cam_dir_since = now + c.lead_ms;
    }
    else if (dir != g_cam_dir) {
        // A new way while the hold buttons are down: its presses start now
        // (or once the lead is over).
        const uint64_t lead_end = g_cam_held_since + c.lead_ms;
        g_cam_dir_since = now > lead_end ? now : lead_end;
    }
    g_cam_dir = dir;
    uint16_t b = c.hold;
    const uint32_t period = c.period_ms > c.press_ms ? c.period_ms : (uint32_t)c.press_ms + 1;
    if (now >= g_cam_dir_since && (now - g_cam_dir_since) % period < c.press_ms) b |= dir;
    return b;
}

// Radial deadzone over the stick's raw counts, the rest of the range scaled
// back to 0..1 so movement still starts at zero and still reaches full.
void stick_value(const circlePosition& p, float dz, float* x, float* y) {
    float fx = (float)p.dx / CPAD_MAX, fy = (float)p.dy / CPAD_MAX;
    const float m = sqrtf(fx * fx + fy * fy);
    if (m <= dz || m <= 0.0f) { *x = 0.0f; *y = 0.0f; return; }
    float k = (m - dz) / (1.0f - dz);
    if (k > 1.0f) k = 1.0f;
    fx *= k / m; fy *= k / m;
    *x = fx > 1.0f ? 1.0f : fx < -1.0f ? -1.0f : fx;
    *y = fy > 1.0f ? 1.0f : fy < -1.0f ? -1.0f : fy;
}

const ButtonMap kDefaultMap[] = {
    { KEY_A, N64_A }, { KEY_B, N64_B }, { KEY_Y | KEY_ZL, N64_Z }, { KEY_X, N64_CUP }, { KEY_L, N64_L },
    { KEY_R | KEY_ZR, N64_R }, { KEY_START, N64_START },
    { KEY_DUP, N64_DUP }, { KEY_DDOWN, N64_DDOWN }, { KEY_DLEFT, N64_DLEFT }, { KEY_DRIGHT, N64_DRIGHT },
};
// The menu can switch maps (main thread) while the game thread reads it:
// pointer and count change together (8 bytes, LDREXD/STREXD).
struct MapRef { const ButtonMap* map; uint32_t count; };
constexpr MapRef kDefaultRef{ kDefaultMap, sizeof(kDefaultMap) / sizeof(kDefaultMap[0]) };
std::atomic<MapRef> g_map{ kDefaultRef };

// The Controls page's keys, in its order (recomp3ds::kMappableKeys).
constexpr u32 kMappable[recomp3ds::kMappableKeys] = {
    KEY_A, KEY_B, KEY_X, KEY_Y, KEY_L, KEY_R, KEY_ZL, KEY_ZR, KEY_DUP, KEY_DDOWN, KEY_DLEFT, KEY_DRIGHT,
};
// What the read map (g_map) is made of: the game's base map (as it
// starts), its current one (a mod mode's, input_set_map), and the player's
// choices. Changed from any thread (the menu's, the game's mod switches)
// under the lock; the made maps rotate through a few buffers, so one the
// game thread may still be reading is not written over by the next change
// (they come at the menu's pace).
MapRef g_base = kDefaultRef, g_cur = kDefaultRef;
uint16_t g_user[recomp3ds::kMappableKeys] = {};
bool g_user_set = false;
constexpr size_t kMadeMax = 40;
ButtonMap g_made[4][kMadeMax];
unsigned g_made_next = 0;
LightLock& map_lock() {
    static LightLock lock = [] { LightLock l; LightLock_Init(&l); return l; }();
    return lock;
}
// Under map_lock: the map the game thread reads.
void remake_map() {
    if (!g_user_set) { g_map.store(g_cur); return; }    // before the settings load: the game's map as it is
    ButtonMap* out = g_made[g_made_next++ & 3];
    size_t n = 0;
    u32 mappable = 0;
    for (int i = 0; i < recomp3ds::kMappableKeys; i++) {
        const u32 k = kMappable[i];
        mappable |= k;
        // A mod mode's map gives this key other buttons than the game's own
        // map does: the mod's (it repurposes the key); else the player's.
        const uint16_t base = recomp3ds::input_map_buttons(g_base.map, g_base.count, k);
        const uint16_t cur = recomp3ds::input_map_buttons(g_cur.map, g_cur.count, k);
        const uint16_t b = cur != base ? cur : g_user[i];
        if (b != 0 && n < kMadeMax) out[n++] = ButtonMap{ k, b };
    }
    // The log, when it changes: each key's N64 button, '*' where a mod
    // mode's map decides it.
    {
        static const char* const kNames[recomp3ds::kMappableKeys] = { "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "Up", "Down", "Left", "Right" };
        static char last[256] = "";
        char line[256];
        int at = 0;
        for (int i = 0; i < recomp3ds::kMappableKeys && at < (int)sizeof(line) - 24; i++) {
            const u32 k = kMappable[i];
            const uint16_t base = recomp3ds::input_map_buttons(g_base.map, g_base.count, k);
            const uint16_t cur = recomp3ds::input_map_buttons(g_cur.map, g_cur.count, k);
            const uint16_t b = cur != base ? cur : g_user[i];
            at += snprintf(line + at, sizeof(line) - (size_t)at, " %s=%s%s", kNames[i], recomp3ds::kN64ChoiceNames[recomp3ds::n64_choice_of(b)], cur != base ? "*" : "");
        }
        if (strcmp(line, last) != 0) {
            strcpy(last, line);
            fprintf(stderr, "recomp3ds: buttons:%s\n", line);
        }
    }
    // Everything else in the current map (START) as it is.
    for (uint32_t i = 0; i < g_cur.count && n < kMadeMax; i++) {
        const u32 keys = g_cur.map[i].keys & ~mappable;
        if (keys != 0) out[n++] = ButtonMap{ keys, g_cur.map[i].n64 };
    }
    g_map.store(MapRef{ out, uint32_t(n) });
}
}

const char* const recomp3ds::kN64ChoiceNames[kN64Choices] = {
    "None", "A", "B", "Z", "START", "L", "R", "D-Up", "D-Down", "D-Left", "D-Right", "C-Up", "C-Down", "C-Left", "C-Right",
};
const uint16_t recomp3ds::kN64ChoiceButtons[kN64Choices] = {
    0, N64_A, N64_B, N64_Z, N64_START, N64_L, N64_R, N64_DUP, N64_DDOWN, N64_DLEFT, N64_DRIGHT, N64_CUP, N64_CDOWN, N64_CLEFT, N64_CRIGHT,
};
int recomp3ds::n64_choice_of(uint16_t n64) {
    for (int i = 0; i < kN64Choices; i++) if (kN64ChoiceButtons[i] == n64) return i;
    for (int i = 1; i < kN64Choices; i++) if (n64 & kN64ChoiceButtons[i]) return i;
    return 0;
}

u32 recomp3ds::input_mappable_key(int i) { return i >= 0 && i < kMappableKeys ? kMappable[i] : 0; }

uint16_t recomp3ds::input_map_buttons(const ButtonMap* map, size_t count, u32 key) {
    uint16_t b = 0;
    for (size_t i = 0; map != nullptr && i < count; i++) if (map[i].keys & key) b |= map[i].n64;
    return b;
}

void recomp3ds::input_set_base_map(const ButtonMap* map, size_t count) {
    LightLock_Lock(&map_lock());
    g_base = (map != nullptr && count != 0) ? MapRef{ map, uint32_t(count) } : kDefaultRef;
    remake_map();
    LightLock_Unlock(&map_lock());
}

void recomp3ds::input_base_map(const ButtonMap** map, size_t* count) {
    LightLock_Lock(&map_lock());
    *map = g_base.map;
    *count = g_base.count;
    LightLock_Unlock(&map_lock());
}

void recomp3ds::input_set_map(const ButtonMap* map, size_t count) {
    if (map == nullptr || count == 0) return;
    LightLock_Lock(&map_lock());
    g_cur = MapRef{ map, uint32_t(count) };
    remake_map();
    LightLock_Unlock(&map_lock());
}

void recomp3ds::input_set_user_buttons(const uint16_t* n64_per_key) {
    LightLock_Lock(&map_lock());
    for (int i = 0; i < kMappableKeys; i++) g_user[i] = n64_per_key[i];
    g_user_set = true;
    remake_map();
    LightLock_Unlock(&map_lock());
}

void recomp3ds::input_set_cstick_buttons(bool on) { g_cstick_buttons = on; }
void recomp3ds::input_set_cstick_camera(uint16_t hold, uint16_t lead_ms, uint16_t press_ms, uint16_t period_ms) {
    g_cam_cfg.store(uint64_t(hold) | (uint64_t(lead_ms) << 16) | (uint64_t(press_ms) << 32) | (uint64_t(period_ms) << 48));
}
void recomp3ds::input_set_cstick_up(bool on) { g_cstick_up = on; }
void recomp3ds::input_set_cstick_down(bool on) { g_cstick_down = on; }

// Motion (gyro aiming), through JibbSmart's GamepadMotionHelpers (MIT, as
// the desktop uses): gyro calibration while the console is still, gravity
// from the accelerometer fused with the gyro, and "player space" gyro -
// turning measured about gravity, so however the console is held, turning
// it turns the aim.
//
// GamepadMotionHelpers works in the PlayStation/SDL frame (X right, Y up,
// Z towards the player; degrees/second, anticlockwise positive; g). The
// 3DS sensors use that frame with Y mirrored, as Azahar's controller
// mapping has it (src/input_common/sdl/sdl_impl.cpp): accelerometer
// (x, -y, z) at 512 counts per g, gyro (-first, second, -third) in its
// shared-memory order. libctru's angularRate names those fields x, z, y
// and calls x roll and y pitch; the first is really pitch, the third roll.
//
// Sampled from the main thread (~60 times a second); the game takes the
// angle turned since it last asked.
namespace {
volatile bool g_gyro_on = false;
float g_gyro_coef = 14.375f;            // counts per degree/second
GamepadMotion g_motion;
LightLock g_motion_lock;
float g_turn_deg = 0.0f, g_pitch_deg = 0.0f;   // since the game last took them
u64 g_motion_tick = 0, g_take_tick = 0;
}

void recomp3ds::input_set_gyro(bool on) {
    if (on == g_gyro_on) return;
    if (on) {
        LightLock_Init(&g_motion_lock);
        HIDUSER_EnableGyroscope();
        HIDUSER_EnableAccelerometer();
        float c = 0.0f;
        if (R_SUCCEEDED(HIDUSER_GetGyroscopeRawToDpsCoefficient(&c)) && c > 0.0f) g_gyro_coef = c;
        g_motion.Reset();
        g_motion.SetCalibrationMode(GamepadMotionHelpers::CalibrationMode::Stillness | GamepadMotionHelpers::CalibrationMode::SensorFusion);
        g_motion_tick = 0;
    }
    else {
        HIDUSER_DisableGyroscope();
        HIDUSER_DisableAccelerometer();
    }
    g_gyro_on = on;
}

// After hidScanInput, on the main thread.
void recomp3ds::input_motion_update() {
    if (!g_gyro_on) return;
    const u64 now = svcGetSystemTick();
    const float dt = g_motion_tick == 0 ? 0.0f : (float)(now - g_motion_tick) / (float)SYSCLOCK_ARM11;
    g_motion_tick = now;
    if (dt <= 0.0f || dt > 0.25f) return;       // the first sample, or after a pause
    angularRate r;
    accelVector a;
    hidGyroRead(&r);
    hidAccelRead(&a);
    const float k = 1.0f / g_gyro_coef;
    g_motion.ProcessMotion(-(float)r.x * k, (float)r.z * k, -(float)r.y * k,
                           (float)a.x / 512.0f, -(float)a.y / 512.0f, (float)a.z / 512.0f, dt);
    float pitch_dps, yaw_dps;                   // yaw: anticlockwise about gravity, as seen from above
    g_motion.GetPlayerSpaceGyro(pitch_dps, yaw_dps);
    LightLock_Lock(&g_motion_lock);
    g_turn_deg += -yaw_dps * dt;
    g_pitch_deg += pitch_dps * dt;
    LightLock_Unlock(&g_motion_lock);
    // Once a second while it moves: what the aim got, and where gravity
    // points in the console's frame (flat on a table: 0, -1, 0).
    static float log_turn = 0.0f, log_up = 0.0f, log_t = 0.0f;
    log_turn += -yaw_dps * dt;
    log_up += pitch_dps * dt;
    log_t += dt;
    if (log_t >= 1.0f) {
        if (std::fabs(log_turn) > 2.0f || std::fabs(log_up) > 2.0f) {
            float gx, gy, gz;
            g_motion.GetGravity(gx, gy, gz);
            fprintf(stderr, "recomp3ds: gyro: turned %.0f deg right, %.0f deg up; gravity %.2f %.2f %.2f\n", log_turn, log_up, gx, gy, gz);
        }
        log_turn = log_up = log_t = 0.0f;
    }
}

// Degrees turned right and tilted up (the top towards the player, looking
// up through the screen) since the last call. Anything older than a
// quarter second is dropped, so first person does not open with a jump.
void recomp3ds::input_take_gyro(float* turn_right_deg, float* look_up_deg) {
    *turn_right_deg = 0.0f;
    *look_up_deg = 0.0f;
    if (!g_gyro_on) return;
    const u64 now = svcGetSystemTick();
    const bool stale = g_take_tick == 0 || now - g_take_tick > SYSCLOCK_ARM11 / 4;
    g_take_tick = now;
    LightLock_Lock(&g_motion_lock);
    if (!stale) { *turn_right_deg = g_turn_deg; *look_up_deg = g_pitch_deg; }
    g_turn_deg = g_pitch_deg = 0.0f;
    LightLock_Unlock(&g_motion_lock);
}

void recomp3ds::input_set_deadzones(int stick_percent, int cstick_percent) {
    g_stick_dz = (float)stick_percent * 0.01f;
    g_cstick_dz = (float)cstick_percent * 0.01f;
}

// A modal prompt owns the pad (and hidScanInput) while it is up.
void recomp3ds::input_set_blocked(bool blocked) { g_blocked = blocked; }

void recomp3ds::input_poll() {
    if (g_blocked) {
        g_held = 0;
        g_cpad = circlePosition{};
        g_cstick = circlePosition{};
        return;
    }
    hidScanInput();
    u32 scripted = recomp3ds::autotest_tick();
    g_held = hidKeysHeld() | scripted;
    hidCircleRead(&g_cpad);
    hidCstickRead(&g_cstick);
    // Scripted stick directions (AUTOTEST STICK_*, CSTICK_*): full deflection.
    if (scripted & (KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT)) {
        g_cpad.dx = (scripted & KEY_CPAD_RIGHT) ? CPAD_MAX : ((scripted & KEY_CPAD_LEFT) ? -CPAD_MAX : 0);
        g_cpad.dy = (scripted & KEY_CPAD_UP) ? CPAD_MAX : ((scripted & KEY_CPAD_DOWN) ? -CPAD_MAX : 0);
    }
    if (scripted & (KEY_CSTICK_UP | KEY_CSTICK_DOWN | KEY_CSTICK_LEFT | KEY_CSTICK_RIGHT)) {
        g_cstick.dx = (scripted & KEY_CSTICK_RIGHT) ? CPAD_MAX : ((scripted & KEY_CSTICK_LEFT) ? -CPAD_MAX : 0);
        g_cstick.dy = (scripted & KEY_CSTICK_UP) ? CPAD_MAX : ((scripted & KEY_CSTICK_DOWN) ? -CPAD_MAX : 0);
    }
}

bool recomp3ds::input_get(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0) {
        return false;
    }
    uint16_t b = 0;
    const u32 h = g_held & ~settings_menu_keys();
    const MapRef m = g_map.load();
    for (uint32_t i = 0; i < m.count; i++) {
        if (h & m.map[i].keys) b |= m.map[i].n64;
    }
    if (g_cstick_buttons) {
        float cx, cy;
        stick_value(g_cstick, g_cstick_dz, &cx, &cy);
        const CstickCamera cam = cam_cfg();
        if (cam.hold != 0) {
            b |= cstick_camera(cam, cx, cy);
        }
        else {
            g_cam_dir = 0;
            if (cy > CSTICK_BUTTON && g_cstick_up) b |= N64_CUP;
            if (cy < -CSTICK_BUTTON && g_cstick_down) b |= N64_CDOWN;
            if (cx < -CSTICK_BUTTON) b |= N64_CLEFT;
            if (cx > CSTICK_BUTTON) b |= N64_CRIGHT;
        }
    }
    b |= touch_pad_held();
    *buttons = b;
    stick_value(g_cpad, g_stick_dz, x, y);
    return true;
}

void recomp3ds::input_get_right_stick(float* x, float* y) {
    stick_value(g_cstick, g_cstick_dz, x, y);
}

u32 recomp3ds::input_raw_held() {
    return g_held;
}

void recomp3ds::input_set_rumble(int controller_num, bool rumble) {
    (void)controller_num; (void)rumble;
}

namespace {
ultramodern::input::Pak g_controller_pak = ultramodern::input::Pak::None;
}

void recomp3ds::input_set_controller_pak(ultramodern::input::Pak pak) {
    g_controller_pak = pak;
}

ultramodern::input::connected_device_info_t recomp3ds::input_device_info(int controller_num) {
    ultramodern::input::connected_device_info_t info{};
    info.connected_device = controller_num == 0 ? ultramodern::input::Device::Controller : ultramodern::input::Device::None;
    info.connected_pak = controller_num == 0 ? g_controller_pak : ultramodern::input::Pak::None;
    return info;
}
