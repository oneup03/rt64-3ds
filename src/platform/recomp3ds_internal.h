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
    // Audio off: the audio task is skipped (no microcode work, which the
    // CPU does here) and silence is queued in place of its output.
    void audio_set_enabled(bool on);
    bool audio_enabled();
    // Since the previous call: frames the game queued, frames dropped because
    // the ring was full, and times the DSP had run dry before new samples came.
    struct AudioCounters { uint32_t submitted, dropped, underruns, rate; };
    AudioCounters audio_take_counters();

    void input_set_deadzones(int stick_percent, int cstick_percent);
    void input_set_cstick_up(bool on);  // C-Stick up presses C-Up (off: nothing)
    void input_poll();
    void input_motion_update();         // main thread, after hidScanInput
    bool input_get(int controller_num, uint16_t* buttons, float* x, float* y);
    void input_get_right_stick(float* x, float* y);
    u32  input_raw_held();
    void input_set_rumble(int controller_num, bool rumble);
    ultramodern::input::connected_device_info_t input_device_info(int controller_num);

    // The ROM read from the SD card as the game needs it (rom_stream.cpp):
    // installs the runtime's RomStream with a block cache of about
    // cache_bytes, before start.
    void rom_stream_install(size_t cache_bytes);
    bool rom_stream_active();
    void rom_stream_take_stats(uint32_t* misses, uint32_t* bytes);   // since the last call

    void log_init(const char* base_path);

    void log_start_writer();

    void log_flush();
    void log_line(const char* s);

    void autotest_load(const char* base_path);
    void autotest_scan_line(const char* line);
    u32  autotest_tick();
    u32  autotest_keys();               // the scripted keys autotest_tick last returned
    bool autotest_touch(int* x, int* y);
    bool autotest_take_home();          // a scripted HOME press since the last call
    void input_set_blocked(bool blocked);
    // The settings menu on the touch screen (settings_menu.cpp).
    struct GameDesc;
    void settings_menu_init(const char* base_path, const GameDesc& desc);
    void set_cpu_speed(bool new3ds);    // the New 3DS clock and L2 on or off (recomp3ds_run.cpp)
    void settings_menu_update();        // ~60 times a second, main thread
    void settings_menu_toggle();
    int settings_rom_stream();          // settings.ini's rom_stream: 0 auto, 1 in memory, 2 from the SD card
    void settings_menu_redraw();        // after something else wrote over the console
    u32  settings_menu_keys();          // 3DS keys the open menu keeps from the game

    void loadmon_start(bool has_core2);
    void loadmon_sample(int* busy0, int* busy2);
    void loadmon_stop();
}

#endif
