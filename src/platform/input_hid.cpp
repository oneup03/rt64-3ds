// ultramodern input callbacks on libctru's HID: one N64 controller from the
// 3DS pad. Map (changeable later through settings):
//   A=A  B=B  Y=Z  ZL=Z  X=C-up  L=L  R/ZR=R  D-pad=D-pad  START=START
//   Circle Pad = stick, C-Stick = C buttons (New 3DS)
#include <3ds.h>
#include <cstring>

#include "ultramodern/input.hpp"
#include "recomp3ds_internal.h"

namespace {
// N64 controller button bits (as OSContPad.button).
constexpr uint16_t N64_A = 0x8000, N64_B = 0x4000, N64_Z = 0x2000, N64_START = 0x1000;
constexpr uint16_t N64_DU = 0x0800, N64_DD = 0x0400, N64_DL = 0x0200, N64_DR = 0x0100;
constexpr uint16_t N64_L = 0x0020, N64_R = 0x0010;
constexpr uint16_t N64_CU = 0x0008, N64_CD = 0x0004, N64_CL = 0x0002, N64_CR = 0x0001;
constexpr int CPAD_MAX = 156;
constexpr int CSTICK_THRESHOLD = 60;
u32 g_held = 0;
circlePosition g_cpad{};
circlePosition g_cstick{};
}

void recomp3ds::input_poll() {
    hidScanInput();
    g_held = hidKeysHeld();
    hidCircleRead(&g_cpad);
    hidCstickRead(&g_cstick);
}

bool recomp3ds::input_get(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0) {
        return false;
    }
    uint16_t b = 0;
    u32 h = g_held;
    if (h & KEY_A) b |= N64_A;
    if (h & KEY_B) b |= N64_B;
    if (h & (KEY_Y | KEY_ZL)) b |= N64_Z;
    if (h & KEY_X) b |= N64_CU;
    if (h & KEY_L) b |= N64_L;
    if (h & (KEY_R | KEY_ZR)) b |= N64_R;
    if (h & KEY_START) b |= N64_START;
    if (h & KEY_DUP) b |= N64_DU;
    if (h & KEY_DDOWN) b |= N64_DD;
    if (h & KEY_DLEFT) b |= N64_DL;
    if (h & KEY_DRIGHT) b |= N64_DR;
    if (g_cstick.dy > CSTICK_THRESHOLD) b |= N64_CU;
    if (g_cstick.dy < -CSTICK_THRESHOLD) b |= N64_CD;
    if (g_cstick.dx < -CSTICK_THRESHOLD) b |= N64_CL;
    if (g_cstick.dx > CSTICK_THRESHOLD) b |= N64_CR;
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
