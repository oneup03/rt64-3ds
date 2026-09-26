// Internal glue between the platform files.
#ifndef RECOMP3DS_INTERNAL_H
#define RECOMP3DS_INTERNAL_H

#include <cstddef>
#include <cstdint>
extern "C" {
#include <3ds/types.h>
#include <3ds/svc.h>
}

#include "ultramodern/input.hpp"

namespace recomp3ds {
    // Milliseconds on the ARM11 system tick (268 MHz whatever the CPU clock).
    // Not libctru's osGetTime: ultramodern's N64 osGetTime shadows that name.
    inline uint64_t wall_ms() { return svcGetSystemTick() / 268123ull; }

    bool audio_init();
    void audio_shutdown();
    void audio_set_frequency(uint32_t freq);
    void audio_queue_samples(int16_t* samples, size_t sample_count);
    size_t audio_frames_remaining();
    void audio_set_volume(float v);

    void input_poll();
    bool input_get(int controller_num, uint16_t* buttons, float* x, float* y);
    void input_get_right_stick(float* x, float* y);
    u32  input_raw_held();
    void input_set_rumble(int controller_num, bool rumble);
    ultramodern::input::connected_device_info_t input_device_info(int controller_num);

    void log_init(const char* base_path);
    void log_line(const char* s);

    void autotest_load(const char* base_path);
    void autotest_scan_line(const char* line);
    u32  autotest_tick();
    bool autotest_touch(int* x, int* y);

    void loadmon_start(bool has_core2);
    void loadmon_sample(int* busy0, int* busy2);
    void loadmon_stop();
}

#endif
