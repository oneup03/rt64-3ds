// Internal glue between the platform files.
#ifndef RECOMP3DS_INTERNAL_H
#define RECOMP3DS_INTERNAL_H

#include <cstddef>
#include <cstdint>
#include <3ds/types.h>

#include "ultramodern/input.hpp"

namespace recomp3ds {
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

    void log_init();
    void log_line(const char* s);
}

#endif
