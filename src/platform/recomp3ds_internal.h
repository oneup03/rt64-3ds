// Internal glue between the platform files.
#ifndef RECOMP3DS_INTERNAL_H
#define RECOMP3DS_INTERNAL_H

#include <cstddef>
#include <cstdint>
#include <string>
extern "C" {
#include <3ds/types.h>
#include <3ds/svc.h>
}

#include "ultramodern/input.hpp"

namespace recomp3ds {
    struct GameDesc;
    // Milliseconds on the ARM11 system tick (268 MHz whatever the CPU clock).
    // Not libctru's osGetTime: ultramodern's N64 osGetTime shadows that name.
    inline uint64_t wall_ms() { return svcGetSystemTick() / 268123ull; }

    bool audio_init();
    void audio_shutdown();
    void audio_set_frequency(uint32_t freq);
    void audio_queue_samples(int16_t* samples, size_t sample_count);
    size_t audio_frames_remaining();
    size_t audio_frames_queued();       // all of it, without the margin (the stats line)
    // The AI stream's clock, in its samples (wrapping at 32 bits): those
    // handed to the DSP since init, and those it has played. A game's audio
    // frame plays when the samples before it have; dsp_synth.cpp schedules
    // its voices' events by it. audio_rate: the rate the AI plays at.
    uint32_t audio_frames_added();
    uint32_t audio_frames_played();
    uint32_t audio_rate();
    void audio_set_margin_frames(int video_frames);   // GameDesc::audio_margin_frames
    void audio_set_volume(float v);
    // Audio off: the audio task is skipped (no microcode work, which the
    // CPU does here) and silence is queued in place of its output.
    void audio_set_enabled(bool on);
    bool audio_enabled();
    // Since the previous call: frames the game queued, frames dropped because
    // the ring was full, and times the DSP had run dry before new samples came.
    // low_water: the fewest frames the DSP still held when the game handed
    // over its next buffer (0xFFFFFFFF: none handed over). Near zero means the
    // DSP ran short even when no underrun was caught (ndsp's status flags lag).
    struct AudioCounters { uint32_t submitted, dropped, underruns, rate, low_water; };
    AudioCounters audio_take_counters();

    void input_set_deadzones(int stick_percent, int cstick_percent);
    void input_set_cstick_up(bool on);  // C-Stick up presses C-Up (off: nothing)
    // GameDesc::cstick_camera_*: hold 0 turns the camera mode off.
    void input_set_cstick_camera(uint16_t hold, uint16_t lead_ms, uint16_t press_ms, uint16_t period_ms);
    void input_poll();
    void input_motion_update();         // main thread, after hidScanInput
    bool input_get(int controller_num, uint16_t* buttons, float* x, float* y);
    void input_get_right_stick(float* x, float* y);
    u32  input_raw_held();
    void input_set_rumble(int controller_num, bool rumble);
    ultramodern::input::connected_device_info_t input_device_info(int controller_num);
    void input_set_controller_pak(ultramodern::input::Pak pak);   // GameDesc::controller_pak

    // The ROM read from the SD card as the game needs it (rom_stream.cpp):
    // installs the runtime's RomStream with a block cache of about
    // cache_bytes, before start.
    void rom_stream_install(size_t cache_bytes);
    // Game modes (game_modes.cpp; nothing without RT64_3DS_STATIC_MODS): the
    // ROM patch handler, and the picker before the game starts (returns the
    // chosen mode's id, empty for the game itself).
    void game_modes_init(const char* base_path, const GameDesc& desc);
    std::string pick_game_mode(const GameDesc& desc);
    // Before the game starts: with a game mode chosen, leaves out the mods
    // GameDesc::mods_not_in_game_modes names, for this session only.
    void game_mode_begin(const GameDesc& desc, const std::string& game_mode);
    bool mod_left_out(const std::string& mod_id);
    bool rom_stream_active();
    void rom_stream_take_stats(uint32_t* misses, uint32_t* bytes);   // since the last call

    // The bottom screen's text console is the platform's own (status panel,
    // settings menu, picker, messages): screen_init takes libctru's console
    // right after consoleInit, and screen_printf writes to it. stdout goes to
    // the log with stderr (log_init), so the runtime's diagnostic printf
    // lines ("Opening mod ...") don't stay on the screen.
    void screen_init();
    void screen_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
    // What the console drew since the last call, out of the cache to the
    // framebuffer the display reads (the main loop, each pass).
    void screen_flush();
    // A red line at the bottom of the touch screen from any thread, put on
    // the display at once ("" clears it): the stall watch's alerts.
    void screen_alert(const char* text, int row = 29);

    void log_init(const char* base_path);

    void log_start_writer();

    void log_flush();
    // How long the log's card write under way has taken (0: none under way).
    uint32_t log_write_stuck_ms();
    // The log's lines so far onto the card itself (fsync), at the writer's
    // next pass: the stall watch's alerts, before the power-off that ends a
    // freeze loses the file's last writes.
    void log_sync();
    void log_line(const char* s);
    // settings.ini's log_level: 2 everything, 1 a summary (no renderer
    // reports, the stats line every 10 s; the New 3DS default), 0 boot,
    // warnings and exit (the Old 3DS default).
    void log_set_level(int level);
    int log_level();

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
    bool settings_debug_rows();         // settings.ini's debug_rows: the touch screen's thread and audio rows
    int settings_cores();               // the Cores row, read at start: 0 auto, 1 two cores with the game on core 1, 2 ... on core 0
    // The core the DSP synthesizer's thread runs on (core 1 when the system
    // grants time there, core 0 when the game has core 1).
    int helper_core();
    // The built-in mods' saved state (settings.ini "mod.*") into the runtime,
    // before the mods load; and the game mode the picker chose last.
    void settings_apply_mods();
    // The touch-screen buttons (touch_pad.cpp): update from the main thread
    // with the touch point (shown: the settings menu is closed; redraw: the
    // console was cleared); the game thread reads what is held.
    void touch_pad_update(bool shown, bool contact, int x, int y, bool redraw);
    uint16_t touch_pad_held();
    std::string settings_game_mode();
    void settings_set_game_mode(const std::string& mod_id);
    bool settings_debug_capture();      // settings.ini's debug_capture: a SELECT hold saves a frame capture
    void settings_menu_redraw();        // after something else wrote over the console
    void settings_flush();              // write settings.ini now if a change is pending (at exit)
    u32  settings_menu_keys();          // 3DS keys the open menu keeps from the game

    void loadmon_start(unsigned core_mask);     // the cores to watch (bit n: core n)
    int loadmon_sample(int core);                // busy % since the last call, -1 if not watched
    int loadmon_peek(int core);                  // the same apart from loadmon_sample's (the stall watch)
    void loadmon_set_mhz(unsigned mhz);          // the CPU clock now (the calibrations scale with it)
    void loadmon_stop();

    // stall_watch.cpp: logs which game thread holds core 0 when the main
    // thread (stall_watch_beat, from update_gfx) has not run for 3 s, and
    // says on the touch screen when that thread, the frames or the log's
    // card writes have stopped for that long.
    void stall_watch_start(uint8_t* const* rdram, const GameDesc* desc);
    // Where the main thread's per-frame work is (update_gfx_inner), for the
    // stall watch's touch-screen line: 0 between passes (sleeping), then
    // 1 the menu and pad, 2 the touch screen's flush, 3 motion, 4 the HOME
    // check, 5 SELECT, 6 the once-a-second report, 7 the GPU watchdog,
    // 8 aptMainLoop.
    int main_stage();
    const char* main_stage_name(int stage);
    void stall_watch_beat();
    void stall_watch_pause(bool paused);   // the HOME prompt holds the game: no alert, no report meanwhile
    void stall_watch_stop();
}

#endif
