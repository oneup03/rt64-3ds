// ultramodern input callbacks on libctru's HID: one N64 controller from the
// 3DS pad. The buttons follow the game's ButtonMap (GameDesc), or by default:
//   A=A  B=B  Y=Z  ZL=Z  X=C-up  L=L  R/ZR=R  D-pad=D-pad  START=START
// The Circle Pad is the stick and the C-Stick (New 3DS) the C buttons.
// SELECT belongs to the platform (settings menu, frame capture), and the
// D-pad to the menu while it is open.
#include <3ds.h>
#include <cstring>

#include "ultramodern/input.hpp"
#include "recomp3ds_internal.h"
#include "recomp3ds.h"

namespace {
using namespace recomp3ds;
constexpr int CPAD_MAX = 156;
constexpr int CSTICK_THRESHOLD = 60;
u32 g_held = 0;
circlePosition g_cpad{};
circlePosition g_cstick{};
volatile bool g_blocked = false;
volatile bool g_cstick_buttons = true;

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
        if (g_cstick.dy > CSTICK_THRESHOLD) b |= N64_CUP;
        if (g_cstick.dy < -CSTICK_THRESHOLD) b |= N64_CDOWN;
        if (g_cstick.dx < -CSTICK_THRESHOLD) b |= N64_CLEFT;
        if (g_cstick.dx > CSTICK_THRESHOLD) b |= N64_CRIGHT;
    }
    *buttons = b;
    float fx = (float)g_cpad.dx / CPAD_MAX;
    float fy = (float)g_cpad.dy / CPAD_MAX;
    if (fx > 1.0f) fx = 1.0f; if (fx < -1.0f) fx = -1.0f;
    if (fy > 1.0f) fy = 1.0f; if (fy < -1.0f) fy = -1.0f;
    *x = fx;
    *y = fy;
    return true;
}

void recomp3ds::input_get_right_stick(float* x, float* y) {
    *x = (float)g_cstick.dx / CPAD_MAX;
    *y = (float)g_cstick.dy / CPAD_MAX;
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
