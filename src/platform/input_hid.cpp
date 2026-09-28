// ultramodern input callbacks on libctru's HID: one N64 controller from the
// 3DS pad. The buttons follow the game's ButtonMap (GameDesc), or by default:
//   A=A  B=B  Y=Z  ZL=Z  X=C-up  L=L  R/ZR=R  D-pad=D-pad  START=START
// The Circle Pad is the stick and the C-Stick (New 3DS) the C buttons.
// SELECT belongs to the platform (settings menu, frame capture), and the
// D-pad to the menu while it is open.
#include <3ds.h>
#include <cmath>
#include <cstring>

#include "ultramodern/input.hpp"
#include "recomp3ds_internal.h"
#include "recomp3ds.h"

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
volatile float g_stick_dz = 0.05f, g_cstick_dz = 0.20f;

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
const ButtonMap* g_map = kDefaultMap;
size_t g_map_count = sizeof(kDefaultMap) / sizeof(kDefaultMap[0]);
}

void recomp3ds::input_set_map(const ButtonMap* map, size_t count) {
    if (map != nullptr && count != 0) { g_map = map; g_map_count = count; }
}

void recomp3ds::input_set_cstick_buttons(bool on) { g_cstick_buttons = on; }

// Gyroscope (for aiming). libctru names the rates x = roll, y = pitch,
// z = yaw, in raw counts; the HID coefficient turns them into degrees per
// second. The resting offset drifts, so it is learned while the console is
// held still (every poll, whether or not anything reads the gyro), and
// motion slower than kGyroStill is taken as drift.
namespace {
volatile bool g_gyro_on = false;
float g_gyro_coef = 14.375f;            // counts per degree/second (the ITG-3200's)
float g_gyro_bias[2] = { 0.0f, 0.0f };  // yaw, pitch counts
float g_gyro_rate[2] = { 0.0f, 0.0f };  // yaw, pitch degrees/second after the bias
bool g_gyro_primed = false;
constexpr float kGyroStill = 2.0f;      // degrees/second
constexpr float kGyroBiasRate = 0.01f;  // per poll (~3 s at 30 polls a second)

void gyro_poll() {
    angularRate r;
    hidGyroRead(&r);
    const float raw[2] = { (float)r.z, (float)r.y };
    if (!g_gyro_primed) { g_gyro_bias[0] = raw[0]; g_gyro_bias[1] = raw[1]; g_gyro_primed = true; }
    for (int a = 0; a < 2; a++) {
        const float dps = (raw[a] - g_gyro_bias[a]) / g_gyro_coef;
        if (std::fabs(dps) < kGyroStill) g_gyro_bias[a] += (raw[a] - g_gyro_bias[a]) * kGyroBiasRate;
        g_gyro_rate[a] = dps;
    }
}
}

void recomp3ds::input_set_gyro(bool on) {
    if (on == g_gyro_on) return;
    if (on) {
        HIDUSER_EnableGyroscope();
        float c = 0.0f;
        if (R_SUCCEEDED(HIDUSER_GetGyroscopeRawToDpsCoefficient(&c)) && c > 0.0f) g_gyro_coef = c;
        g_gyro_primed = false;
    }
    else {
        HIDUSER_DisableGyroscope();
        g_gyro_rate[0] = g_gyro_rate[1] = 0.0f;
    }
    g_gyro_on = on;
}

// Turning the console right is positive yaw, tilting its top towards you
// (looking up through it) positive pitch.
void recomp3ds::input_get_gyro(float* yaw_dps, float* pitch_dps) {
    *yaw_dps = g_gyro_on ? -g_gyro_rate[0] : 0.0f;
    *pitch_dps = g_gyro_on ? g_gyro_rate[1] : 0.0f;
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
    if (g_gyro_on) gyro_poll();
    // Scripted stick directions (AUTOTEST STICK_*): full deflection.
    if (scripted & (KEY_CPAD_UP | KEY_CPAD_DOWN | KEY_CPAD_LEFT | KEY_CPAD_RIGHT)) {
        g_cpad.dx = (scripted & KEY_CPAD_RIGHT) ? CPAD_MAX : ((scripted & KEY_CPAD_LEFT) ? -CPAD_MAX : 0);
        g_cpad.dy = (scripted & KEY_CPAD_UP) ? CPAD_MAX : ((scripted & KEY_CPAD_DOWN) ? -CPAD_MAX : 0);
    }
}

bool recomp3ds::input_get(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0) {
        return false;
    }
    uint16_t b = 0;
    const u32 h = g_held & ~settings_menu_keys();
    for (size_t i = 0; i < g_map_count; i++) {
        if (h & g_map[i].keys) b |= g_map[i].n64;
    }
    if (g_cstick_buttons) {
        float cx, cy;
        stick_value(g_cstick, g_cstick_dz, &cx, &cy);
        if (cy > CSTICK_BUTTON) b |= N64_CUP;
        if (cy < -CSTICK_BUTTON) b |= N64_CDOWN;
        if (cx < -CSTICK_BUTTON) b |= N64_CLEFT;
        if (cx > CSTICK_BUTTON) b |= N64_CRIGHT;
    }
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

ultramodern::input::connected_device_info_t recomp3ds::input_device_info(int controller_num) {
    ultramodern::input::connected_device_info_t info{};
    info.connected_device = controller_num == 0 ? ultramodern::input::Device::Controller : ultramodern::input::Device::None;
    info.connected_pak = ultramodern::input::Pak::None;
    return info;
}
