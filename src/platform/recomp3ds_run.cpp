// The 3DS entry point: console services, the runtime's callbacks, and the
// runtime itself. The game supplies a GameDesc; everything else is shared.
#include <3ds.h>
#include <malloc.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/stat.h>

#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/error_handling.hpp"
#include "ultramodern/events.hpp"
#include "ultramodern/host_thread.hpp"
#include "recomp3ds.h"
#include "recomp3ds_internal.h"
#include "naudio_hle.h"
#include "dsp_synth.h"
#include "aspmain_hle.h"
#include "audio_hle_diff.h"

// libctru's default main stack is 32 KB; the runtime's start path and the
// C++ library want more.
extern "C" { u32 __stacksize__ = 512 * 1024; }

namespace {

char g_base_path[128] = "sdmc:/3ds";

// RSP task timing (audio: the graphics tasks go to the renderer instead).
recomp::rsp::callbacks_t::get_rsp_microcode_t* g_game_get_ucode = nullptr;
RspUcodeFunc* g_ucode_inner = nullptr;
u64 g_ucode_ticks = 0;
u32 g_ucode_tasks = 0;

bool g_audio_hle = false;
recomp3ds::AudioHle g_audio_hle_family = recomp3ds::AudioHle::Naudio;

uint8_t* g_rdram = nullptr;
// SLOW.TXT "<gfx ms> <audio ms>": pad every display list and audio task by
// that much, to reproduce console timing in the emulator.
int g_slow_audio_ms = -1;

RspExitReason timed_ucode(uint8_t* rdram, uint32_t ucode_addr) {
    g_rdram = rdram;
    if (recomp3ds::audio_hle_capturing()) {
        // AUDIO_CAPTURE.TXT: the task as it arrives, whichever code runs it.
        OSTask task;
        memcpy(&task, &dmem[0xFC0], sizeof(OSTask));
        if (task.t.type == M_AUDTASK) recomp3ds::audio_hle_maybe_capture(rdram, task);
    }
    u64 t0 = svcGetSystemTick();
    RspExitReason r = g_ucode_inner(rdram, ucode_addr);
    if (g_slow_audio_ms > 0) svcSleepThread((s64)g_slow_audio_ms * 1000000);
    g_ucode_ticks += svcGetSystemTick() - t0;
    g_ucode_tasks++;
    return r;
}

// Audio off: the audio task completes at once, doing nothing.
RspExitReason audio_off_ucode(uint8_t*, uint32_t) {
    return RspExitReason::Broke;
}

RspUcodeFunc* timed_get_ucode(const OSTask* task) {
    // Audio off, or the voices on the DSP (dsp_synth.h): the task does nothing
    // and the AI stream carries silence, at the usual rate.
    if (task->t.type == M_AUDTASK && (!recomp3ds::audio_enabled() || recomp3ds::dsp_synth_enabled())) {
        return audio_off_ucode;
    }
    RspUcodeFunc* f = g_game_get_ucode(task);
    if (f == nullptr) {
        return nullptr;
    }
    // The first microcode the game hands over is its audio task's.
    static RspUcodeFunc* game_ucode = nullptr;
    if (game_ucode == nullptr) {
        game_ucode = f;
        char path[192];
        snprintf(path, sizeof(path), "%s/AUDIO_CAPTURE.TXT", g_base_path);
        if (FILE* c = fopen(path, "r")) {
            int first = 0;
            if (fscanf(c, "%d", &first) != 1) first = 0;   // the first task to capture
            fclose(c);
            recomp3ds::audio_hle_capture_dir(g_base_path, first);
        }
        if (g_audio_hle) {
            // The CPU interpreter replaces the game's (recompiled) microcode. In
            // differential mode it still runs the original to check itself.
            const bool aspmain = g_audio_hle_family == recomp3ds::AudioHle::Aspmain;
            snprintf(path, sizeof(path), "%s/AUDIO_RSP.TXT", g_base_path);
            if (FILE* r = fopen(path, "r")) {
                fclose(r);
                g_audio_hle = false;   // the recompiled microcode itself, for comparison
                fprintf(stderr, "recomp3ds: audio runs the recompiled RSP microcode (AUDIO_RSP.TXT present)\n");
            }
            snprintf(path, sizeof(path), "%s/AUDIO_DIFF.TXT", g_base_path);
            if (g_audio_hle) {
                if (FILE* d = fopen(path, "r")) {
                    fclose(d);
                    if (aspmain) recomp3ds::aspmain_hle_set_reference(f);
                    else recomp3ds::naudio_hle_set_reference(f);
                    fprintf(stderr, "recomp3ds: audio HLE (%s) in differential mode (AUDIO_DIFF.TXT present)\n", aspmain ? "aspMain" : "naudio");
                }
                else {
                    fprintf(stderr, "recomp3ds: audio task interpreted on the CPU (%s; AUDIO_RSP.TXT selects the recompiled microcode)\n",
                            aspmain ? "aspMain" : "naudio");
                }
            }
        }
    }
    if (g_audio_hle && f == game_ucode) {
        f = g_audio_hle_family == recomp3ds::AudioHle::Aspmain ? recomp3ds::aspmain_hle_run : recomp3ds::naudio_hle_run;
    }
    if (g_ucode_inner != nullptr && g_ucode_inner != f) {
        return f;   // a second microcode: leave it untimed rather than mix them
    }
    g_ucode_inner = f;
    return timed_ucode;
}

extern "C" void __appExit(void);   // libctru: closes the services __appInit opened
extern "C" void (*__system_retAddr)(void);   // libctru: set by aptExit to finish closing the application

const recomp3ds::GameDesc* g_desc = nullptr;
int g_sp_core = 2;
int g_gfx_core = 2;                     // core 1 or 0 on an Old 3DS, which has no core 2
int g_game_core = 0;                    // the N64 threads (the Cores row's "game on 1": core 1)
int g_helper_core = 0;                  // the DSP synthesizer's decoding thread
unsigned g_watch_cores = 1;             // the cores loadmon watches (bit n: core n)
bool g_core3 = false;                   // a New 3DS lets the application run threads on core 3
bool g_core1_granted = false;           // the system gives the application time on core 1
volatile int g_main_stage = 0;          // recomp3ds::main_stage

// Thread placement: game code on core 0, renderer on core 2 (on an Old 3DS
// core 1 when the system grants time there, else 0), the audio task on
// g_sp_core (core 1 when the system grants time there), pacing threads
// above the game so their sleeps wake it, on its core. The Cores row's
// "game on 1" swaps: the game on core 1, everything else on core 0.
ultramodern::HostThreadSpec host_thread_spec(ultramodern::HostThreadKind kind) {
    using K = ultramodern::HostThreadKind;
    switch (kind) {
        case K::GameStart:
        case K::Game:    return { 128 * 1024, 0x30, g_game_core };
        case K::Gfx:     return { 256 * 1024, 0x2C, g_gfx_core };
        case K::SpTask:  return { 64 * 1024, 0x24, g_sp_core };
        case K::Vi:
        case K::Timer:   return { 32 * 1024, 0x28, g_game_core };
        default:         return { 32 * 1024, 0x30, g_game_core };
    }
}
volatile bool g_exit_requested = false;
void (*g_game_on_init)(uint8_t* rdram, recomp_context* ctx) = nullptr;

}   // namespace

int recomp3ds::helper_core() { return g_helper_core; }
int recomp3ds::main_stage() { return g_main_stage; }
const char* recomp3ds::main_stage_name(int stage) {
    static const char* const kNames[] = { "sleep", "pad", "flush", "motion", "home", "select", "report", "watchdog", "apt" };
    return stage >= 0 && stage < 9 ? kNames[stage] : "?";
}

namespace {

void* create_gfx() { return nullptr; }

ultramodern::renderer::WindowHandle create_window(void*) {
    return ultramodern::renderer::WindowHandle{};
}

// The end of quitting, from run() once the runtime has returned or from the
// exit watchdog: libctru's exit minus C++ destructors and the heap unmapping,
// which would pull memory from under the game threads the runtime leaves
// blocked (a data abort on the way out). __appExit's aptExit sends
// PrepareToClose and leaves the final APT_CloseApplication in
// __system_retAddr, which __libctru_exit calls just before svcExitProcess;
// skipping that left the HOME Menu with a half-closed application.
[[noreturn]] void finish_exit() {
    static volatile u32 once = 0;
    if (__atomic_exchange_n(&once, 1u, __ATOMIC_SEQ_CST) != 0) {
        for (;;) svcSleepThread(1000000000ll);    // the other path is already leaving
    }
    fflush(stderr);
    // A frame capture still being written stops between two FS requests
    // (its file stays incomplete): nothing of ours is in flight on the card
    // when FS is torn down below.
    rt64_3ds::capture_stop(1500);
    recomp3ds::log_flush();
    recomp3ds::stall_watch_stop();
    recomp3ds::loadmon_stop();
    recomp3ds::dsp_synth_shutdown();
    recomp3ds::audio_shutdown();
    gfxExit();
    osSetSpeedupEnable(false);             // hand the system back at the clock it had
    __appExit();
    if (__system_retAddr) __system_retAddr();
    svcExitProcess();
}

// HOME: a quit prompt on the touch screen instead of going straight to the
// HOME Menu (as in the Sonic R port). A quits, B resumes, HOME again goes to
// the HOME Menu. While it is up the game is held (the renderer takes no
// display list, so the game waits on its current one), its input is blocked
// and sound is muted; the top screen keeps the last frame. It returns only
// once B is released, so that press does not reach the game.
bool g_home_trapped = false;

u32 prompt_keys() {
    hidScanInput();
    return hidKeysHeld() | recomp3ds::autotest_tick();
}

void draw_home_prompt(bool show) {
    constexpr int kRow = 23;
    if (show) {
        recomp3ds::screen_printf("\x1b[%d;0H----------------------------------------", kRow);
        recomp3ds::screen_printf("\x1b[%d;0H  Quit %-33s", kRow + 1, g_desc != nullptr ? g_desc->render.game_name : "the game");
        recomp3ds::screen_printf("\x1b[%d;0H  A: quit   B: resume   HOME: HOME Menu ", kRow + 2);
        recomp3ds::screen_printf("\x1b[%d;0H----------------------------------------", kRow + 3);
    }
    else {
        for (int r = kRow; r < kRow + 4; r++) recomp3ds::screen_printf("\x1b[%d;0H%40s", r, "");
    }
    // The console writes through the CPU cache; with the game (and so the
    // renderer's frame flushes) paused, flush them to the screen here.
    gfxFlushBuffers();
}

// Returns true when the player chose to quit.
bool home_prompt() {
    fprintf(stderr, "recomp3ds: HOME - quit prompt\n");
    rt64_3ds::set_paused(true);
    recomp3ds::input_set_blocked(true);
    ndspSetMasterVol(0.0f);
    draw_home_prompt(true);
    u32 prev = prompt_keys();          // held on entry: not a press
    bool quit = false, to_home = false;
    for (;;) {
        if (!aptMainLoop()) { quit = true; break; }
        const u32 keys = prompt_keys();
        const u32 down = keys & ~prev;
        prev = keys;
        if (aptCheckHomePressRejected() || recomp3ds::autotest_take_home()) { to_home = true; break; }
        if (down & KEY_A) { quit = true; break; }
        if (down & KEY_B) break;
        svcSleepThread(16 * 1000000ll);
    }
    if (to_home) {
        fprintf(stderr, "recomp3ds: HOME prompt -> HOME Menu\n");
        aptJumpToHomeMenu();           // returns when the game is resumed (or is to close)
    }
    else if (!quit) {
        while ((prompt_keys() & KEY_B) != 0) {
            if (!aptMainLoop()) { quit = true; break; }
            svcSleepThread(16 * 1000000ll);
        }
    }
    draw_home_prompt(false);
    recomp3ds::settings_menu_redraw();
    ndspSetMasterVol(1.0f);
    recomp3ds::input_set_blocked(false);
    if (!quit) rt64_3ds::set_paused(false);   // quitting releases the renderer itself
    fprintf(stderr, "recomp3ds: HOME prompt closed (%s)\n", quit ? "quit" : to_home ? "HOME Menu" : "resume");
    return quit;
}

// Called by the runtime's main loop (which sleeps 1 ms between calls) on
// core 0, the game's core: it sleeps most of a VI itself so the main thread
// wakes ~60 times a second instead of 1000.
void update_gfx_inner();
void update_gfx(void*) {
    update_gfx_inner();
    g_main_stage = 0;
    if (!g_exit_requested) svcSleepThread(15 * 1000000ll);
}

void update_gfx_inner() {
    recomp3ds::stall_watch_beat();      // the main thread ran (stall_watch.cpp)
    g_main_stage = 1;
    recomp3ds::settings_menu_update();  // scans the pad
    g_main_stage = 2;
    recomp3ds::screen_flush();
    g_main_stage = 3;
    recomp3ds::input_motion_update();
    g_main_stage = 4;
    // HOME opens the quit prompt (above) rather than the HOME Menu.
    if (!g_home_trapped) {
        aptSetHomeAllowed(false);
        g_home_trapped = true;
    }
    if (!g_exit_requested && (aptCheckHomePressRejected() || recomp3ds::autotest_take_home())) {
        if (home_prompt()) g_exit_requested = true;
    }
    g_main_stage = 5;
    {
        // SELECT (the game has no use for it): a tap opens or closes the
        // settings menu; held for a second it saves a screenshot (with
        // debug_capture=1 in settings.ini, a frame capture for the host
        // tools instead), and says so on the touch screen.
        static u64 select_since = 0;
        static bool fired = false;
        if ((hidKeysHeld() | recomp3ds::autotest_keys()) & KEY_SELECT) {
            const u64 now = svcGetSystemTick();
            if (select_since == 0) select_since = now;
            else if (!fired && now - select_since > SYSCLOCK_ARM11) {
                fired = true;
                if (recomp3ds::settings_debug_capture()) {
                    rt64_3ds::request_capture();
                    recomp3ds::screen_printf("\x1b[28;0H%-39s", "Saving a frame capture...");
                }
                else {
                    rt64_3ds::request_screenshot();
                    recomp3ds::screen_printf("\x1b[28;0H%-39s", "Saving a screenshot...");
                }
                gfxFlushBuffers();
            }
        }
        else {
            if (select_since != 0 && !fired) recomp3ds::settings_menu_toggle();
            select_since = 0;
            fired = false;
        }
        // The capture writer's news: saving, progress, saved, refused (one
        // is still being written), failed or stalled.
        static int shown_seq = 0, shown_shot = 0;
        int seq = 0;
        const char* capture_line = rt64_3ds::capture_message(&seq);
        if (seq != shown_seq) {
            shown_seq = seq;
            recomp3ds::screen_printf("\x1b[28;0H%-39.39s", capture_line);
            gfxFlushBuffers();
        }
        const int shot = rt64_3ds::screenshot_state();
        if (shot >= 2 && shown_shot != shot) {
            char line[48];
            if (shot == 2) snprintf(line, sizeof(line), "Screenshot %s", rt64_3ds::screenshot_name());
            else snprintf(line, sizeof(line), "Screenshot failed");
            recomp3ds::screen_printf("\x1b[28;0H%-39.39s", line);
            gfxFlushBuffers();
        }
        shown_shot = shot;
    }
    g_main_stage = 6;
    // Once a second: frame rate and CPU load to the log and the touch screen.
    static u64 last_report = 0;
    u64 now = svcGetSystemTick();
    if (last_report == 0) {
        last_report = now;
    }
    else if (now - last_report >= SYSCLOCK_ARM11) {
        const u64 interval = now - last_report;
        last_report = now;
        const int busy0 = recomp3ds::loadmon_sample(0), busy1 = recomp3ds::loadmon_sample(1);
        const int busy2 = recomp3ds::loadmon_sample(2), busy3 = recomp3ds::loadmon_sample(3);
        const rt64_3ds::FrameStats& st = rt64_3ds::stats();
        // Audio microcode cost over the last second: ms of CPU and task count.
        unsigned ucode_ms = (unsigned)(g_ucode_ticks * 1000 / SYSCLOCK_ARM11);
        unsigned ucode_tasks = g_ucode_tasks;
        g_ucode_ticks = 0;
        g_ucode_tasks = 0;
        // Audio stall watchdog: tasks ran, then none for 2 s.
        {
            static bool seen = false, reported = false;
            static int idle = 0;
            static int test_countdown = -2;   // STALL_DUMP_TEST.TXT: run the dump once after 30 s (checks the dumper)
            if (test_countdown == -2) {
                char path[192];
                snprintf(path, sizeof(path), "%s/STALL_DUMP_TEST.TXT", g_base_path);
                FILE* f = fopen(path, "r");
                test_countdown = f ? 30 : -1;
                if (f) fclose(f);
            }
            if (test_countdown > 0 && --test_countdown == 0 && g_desc != nullptr && g_desc->on_audio_stall != nullptr && g_rdram != nullptr) {
                fprintf(stderr, "recomp3ds: stall dump test\n");
                g_desc->on_audio_stall(g_rdram);
            }
            if (ucode_tasks > 0) { seen = true; idle = 0; }
            else if (seen && !reported && ++idle >= 2) {
                reported = true;
                fprintf(stderr, "recomp3ds: AUDIO STALL: no audio task for 2 s\n");
                if (g_desc != nullptr && g_desc->on_audio_stall != nullptr && g_rdram != nullptr) g_desc->on_audio_stall(g_rdram);
                fflush(stderr);
            }
        }
        const recomp3ds::AudioCounters ac = recomp3ds::audio_take_counters();
        char game_stats[96] = "";
        if (g_desc != nullptr && g_desc->append_stats != nullptr && g_rdram != nullptr) {
            g_desc->append_stats(g_rdram, game_stats, sizeof(game_stats));
        }
        char rom_stats[64] = "";
        if (recomp3ds::rom_stream_active()) {
            uint32_t misses = 0, bytes = 0;
            recomp3ds::rom_stream_take_stats(&misses, &bytes);
            snprintf(rom_stats, sizeof(rom_stats), " rom %lu reads (%lu KB)", (unsigned long)misses, (unsigned long)(bytes / 1024));
        }
        // The audio interpreter's unknown commands and, with AUDIO_DIFF.TXT,
        // its mismatches against the recompiled microcode: shown when non-zero.
        char hle_stats[96] = "";
        {
            const recomp3ds::AudioHleStats& hs = g_audio_hle_family == recomp3ds::AudioHle::Aspmain ? recomp3ds::aspmain_hle_stats()
                                                                                                    : recomp3ds::naudio_hle_stats();
            if (hs.unknown_opcodes != 0 || hs.diff_mismatches != 0)
                snprintf(hle_stats, sizeof(hle_stats), " audio-hle unknown %lu mismatches %lu/%lu", (unsigned long)hs.unknown_opcodes,
                         (unsigned long)hs.diff_mismatches, (unsigned long)hs.diff_tasks);
            if (hs.inexact != 0) {
                const size_t used = strlen(hle_stats);
                snprintf(hle_stats + used, sizeof(hle_stats) - used, " inexact %lu", (unsigned long)hs.inexact);
            }
        }
        // The audio interpreter's time by command (aspMain), ms in the
        // interval: which of its loops a faster interpreter must target.
        std::string hle_ms;
        char hle_row[48] = "";
        if (g_audio_hle && g_audio_hle_family == recomp3ds::AudioHle::Aspmain) {
            static const char* const kOps[16] = { "nop", "adpcm", "clear", "envmix", "loadbuf", "resamp", "savebuf", "segment",
                                                  "setbuff", "setvol", "dmemmv", "loadadp", "mixer", "interlv", "polef", "setloop" };
            static uint32_t prev_ticks[16] = {};
            const recomp3ds::AudioHleStats& hs = recomp3ds::aspmain_hle_stats();
            unsigned ms[16], order[16];
            for (int i = 0; i < 16; i++) {
                ms[i] = (unsigned)((uint64_t)(uint32_t)(hs.op_ticks[i] - prev_ticks[i]) * 1000 / SYSCLOCK_ARM11);
                prev_ticks[i] = hs.op_ticks[i];
                order[i] = (unsigned)i;
            }
            for (int i = 1; i < 16; i++)      // by time, largest first
                for (int j = i; j > 0 && ms[order[j]] > ms[order[j - 1]]; j--) { const unsigned t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
            size_t r = 0;
            for (int i = 0; i < 16 && ms[order[i]] > 0; i++) {
                hle_ms += (hle_ms.empty() ? " hle ms " : " ") + std::string(kOps[order[i]]) + " " + std::to_string(ms[order[i]]);
                if (i < 5 && r + 10 < sizeof(hle_row)) r += (size_t)snprintf(hle_row + r, sizeof(hle_row) - r, "%s%.2s%u", r ? " " : "hle ", kOps[order[i]], ms[order[i]]);
            }
        }
        // Each game thread's share of the interval and its wake-ups a second
        // (ultramodern's accounting at the thread switches): how much of
        // core 0 is the game's logic, its audio synthesizer, its scheduler;
        // what loadmon's core 0 figure has beyond the total is the host's
        // own threads and the switches themselves.
        std::string thread_stats;
        char thread_row[48] = "";
        {
            ultramodern::ThreadLoad loads[16];
            static ultramodern::ThreadLoad prev[16];
            static size_t prev_n = 0;
            const size_t n = ultramodern::get_thread_loads(loads, 16);
            const u64 span = interval != 0 ? interval : 1;
            unsigned total_pct = 0, total_wakes = 0;
            size_t r = 0;
            for (size_t i = 0; i < n; i++) {
                uint32_t ticks = loads[i].ticks, wakeups = loads[i].wakeups;
                for (size_t p = 0; p < prev_n; p++) {
                    if (prev[p].thread == loads[i].thread) { ticks -= prev[p].ticks; wakeups -= prev[p].wakeups; break; }
                }
                const unsigned pct = (unsigned)((u64)ticks * 100 / span);
                const unsigned wakes = (unsigned)((u64)wakeups * SYSCLOCK_ARM11 / span);
                total_pct += pct;
                total_wakes += wakes;
                if (pct == 0 && wakes == 0) continue;    // idle: left out
                std::string name = std::to_string(loads[i].thread);
                if (g_rdram != nullptr && g_desc != nullptr && g_desc->get_game_thread_name != nullptr) {
                    name = g_desc->get_game_thread_name(reinterpret_cast<const OSThread*>(g_rdram + (loads[i].thread & 0x7FFFFFFFu)));
                    if (name.rfind("[Game] ", 0) == 0) name.erase(0, 7);
                }
                thread_stats += (thread_stats.empty() ? " threads " : ", ") + name + " " + std::to_string(pct) + "% (" + std::to_string(wakes) + "/s)";
                if (r + 12 < sizeof(thread_row)) r += (size_t)snprintf(thread_row + r, sizeof(thread_row) - r, "%s%.5s %u", r ? " " : "", name.c_str(), pct);
            }
            if (n > 0) thread_stats += " = " + std::to_string(total_pct) + "%, " + std::to_string(total_wakes) + " wakes/s";
            if (r + 10 < sizeof(thread_row)) snprintf(thread_row + r, sizeof(thread_row) - r, " sw%u", total_wakes);
            memcpy(prev, loads, n * sizeof(loads[0]));
            prev_n = n;
        }
        char dsp_stats[256] = "";
        if (recomp3ds::dsp_synth_enabled()) {
            const recomp3ds::DspSynthStats ds = recomp3ds::dsp_synth_take_stats();
            snprintf(dsp_stats, sizeof(dsp_stats), " dsp-synth %lu playing, %lu starts, %lu events, busy %lu us (decode %lu, events %lu), %lu starved, %lu ms late, %lu remaps, %lu callbacks, %lu ndsp-dropped, reverb %lu us %lu gaps, %lu lists skipped",
                     (unsigned long)ds.playing, (unsigned long)ds.starts, (unsigned long)ds.events, (unsigned long)ds.decode_us,
                     (unsigned long)ds.decode_only_us, (unsigned long)ds.apply_us, (unsigned long)ds.starved, (unsigned long)ds.late,
                     (unsigned long)ds.remaps, (unsigned long)ds.callbacks, (unsigned long)ds.ndsp_dropped,
                     (unsigned long)ds.reverb_us, (unsigned long)ds.reverb_gaps, (unsigned long)ds.lists_skipped);
        }
        char want_fps[240] = "";
        if (st.game_fps > 0) snprintf(want_fps, sizeof(want_fps), ", game asks %d", st.game_fps);
        if (st.dl_skipped_per_sec > 0) {
            // The frame cap passed over some: how many were drawn.
            const size_t used = strlen(want_fps);
            snprintf(want_fps + used, sizeof(want_fps) - used, ", %d drawn, %d passed over", st.dl_per_sec - st.dl_skipped_per_sec, st.dl_skipped_per_sec);
        }
        if (rt64_3ds::settings().frame_cap == 2 && g_desc != nullptr && g_desc->render.frame_interpolation) {
            // Frame interpolation: frames shown, the in-betweens among them,
            // and the lists the game asked 30 for that got none.
            const size_t used = strlen(want_fps);
            snprintf(want_fps + used, sizeof(want_fps) - used, ", %d shown (%d in-between, %d without%s%s)", st.presented_per_sec, st.inbetween_per_sec,
                     st.inbetween_dropped_per_sec, st.inbetween_reasons[0] ? ": " : "", st.inbetween_reasons);
        }
        // log_level 1 logs the figures every 10 s, 0 not at all (log.cpp).
        static unsigned stats_tick = 0;
        stats_tick++;
        const int log_lv = recomp3ds::log_level();
        const bool log_stats = log_lv >= 2 || (log_lv == 1 && stats_tick % 10 == 0);
        // Core 2 when watched (not with two cores), and a New 3DS's core 3
        // when the system lets the application use it.
        char core2[16] = "", core3[16] = "";
        if (busy2 >= 0) snprintf(core2, sizeof(core2), " core2 %d%%", busy2);
        if (busy3 >= 0) snprintf(core3, sizeof(core3), " core3 %d%%", busy3);
        if (log_stats) fprintf(stderr, "stats: %d dl/s%s (late %d, max gap %.0f ms) core0 %d%% core1 %d%%%s%s ucode %u ms/s in %u tasks audio %zu frames queued, in %lu/s at %lu Hz, underruns %lu dropped %lu low water %lu heap %lu KB%s%s%s%s%s%s\n",
                st.dl_per_sec, want_fps, st.late_frames, st.max_gap_ms, busy0, busy1, core2, core3, ucode_ms, ucode_tasks, recomp3ds::audio_frames_queued(),
                (unsigned long)ac.submitted, (unsigned long)ac.rate, (unsigned long)ac.underruns, (unsigned long)ac.dropped, (unsigned long)(ac.low_water == 0xFFFFFFFFu ? 0u : ac.low_water),
                (unsigned long)(mallinfo().arena / 1024), hle_stats, hle_ms.c_str(), "", rom_stats, game_stats, thread_stats.c_str());
        // On a line of its own: the log cuts lines at 512 characters.
        if (log_stats && dsp_stats[0] != '\0') fprintf(stderr, "%s\n", dsp_stats + 1);
        // One 40-column row: frames drawn and each core's load.
        char line[64];
        // Frames drawn (the frame cap passes some over; frame interpolation
        // adds its in-betweens).
        const int shown_fps = rt64_3ds::settings().frame_cap == 2 && g_desc != nullptr && g_desc->render.frame_interpolation
                                  ? st.presented_per_sec : st.dl_per_sec - st.dl_skipped_per_sec;
        if (busy2 >= 0) snprintf(line, sizeof(line), "%3d fps cpu0 %3d%% cpu1 %3d%% cpu2 %3d%%", shown_fps, busy0, busy1, busy2);
        else snprintf(line, sizeof(line), "%3d fps cpu0 %3d%% cpu1 %3d%%", shown_fps, busy0, busy1);   // two cores
        recomp3ds::screen_printf("\x1b[2;0H%-39.39s\n", line);
        // settings.ini's debug_rows=1: the game threads' shares (wake-ups a
        // second last) and the audio rows below.
        const bool debug_rows = recomp3ds::settings_debug_rows();
        if (debug_rows) recomp3ds::screen_printf("\x1b[3;0H%-39.39s", thread_row);
        // The audio pipeline's counters: tasks run a second, frames the game
        // handed over a second against the rate, frames the DSP still holds,
        // underruns and frames dropped for a full ring.
        // lw: the fewest frames the DSP held when a buffer arrived (near 0:
        // it ran short); q: frames queued now; ur: underruns caught.
        const unsigned long low_water = ac.low_water == 0xFFFFFFFFu ? 0ul : (unsigned long)ac.low_water;
        snprintf(line, sizeof(line), "aud %ut in %lu q %zu lw %lu ur %lu", ucode_tasks, (unsigned long)ac.submitted,
                 recomp3ds::audio_frames_queued(), low_water, (unsigned long)ac.underruns);
        if (debug_rows) {
            recomp3ds::screen_printf("\x1b[27;0H%-39.39s", line);
            recomp3ds::screen_printf("\x1b[29;0H%-39.39s", hle_row);   // the interpreter's ms a second by command
        }
    }
    // Watchdog: a frame that stays in the replay or the GPU wait for 4 s is
    // a hung GPU; say which draw it was on, once.
    g_main_stage = 7;
    {
        static u32 last_frames = 0; static u64 since = 0; static bool reported = false;
        const rt64_3ds::Progress& pg = rt64_3ds::progress();
        if (pg.frames != last_frames || pg.phase == 0 || pg.phase == 1) { last_frames = pg.frames; since = now; }
        else if (!reported && since != 0 && now - since > 4ull * SYSCLOCK_ARM11) {
            reported = true;
            fprintf(stderr, "recomp3ds: WATCHDOG: no frame for 4 s\n");
            rt64_3ds::dump_progress();
        }
    }
    g_main_stage = 8;
    if (!aptMainLoop() || g_exit_requested) {
        static bool quitting = false;
        if (!quitting) {
            quitting = true;
            fprintf(stderr, "recomp3ds: quitting\n");
            rt64_3ds::set_quitting();      // no more frames: the renderer's thread can leave its loop
            ultramodern::quit();
            // The runtime joins its own threads and recomp::start returns
            // (run() then finishes the exit). Should a join ever hang, leave
            // anyway rather than keep the HOME Menu on "closing".
            threadCreate([](void*) {
                svcSleepThread(3000000000ll);
                fprintf(stderr, "recomp3ds: exit forced after 3 s\n");
                rt64_3ds::set_quitting();
                svcSleepThread(300000000ll);      // the renderer finishes its frame and goes idle
                finish_exit();
            }, nullptr, 16 * 1024, 0x20, -2, true);
        }
    }
}

// The runtime shows a message box just before it quits, so everything logged
// goes onto the card now: the exit that follows may not get that far.
void message_box(const char* msg) {
    fprintf(stderr, "[message] %s\n", msg);
    recomp3ds::screen_printf("\n%s\n", msg);
    recomp3ds::log_flush();
}

std::string game_thread_name(const OSThread* t) {
    if (g_desc != nullptr && g_desc->get_game_thread_name != nullptr) {
        return g_desc->get_game_thread_name(t);
    }
    return "Game Thread " + std::to_string(t->id);
}

std::unique_ptr<ultramodern::renderer::RendererContext>
make_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle handle, bool developer_mode) {
    if (g_desc != nullptr && g_desc->null_renderer) {
        return rt64_3ds::create_null_render_context(rdram, handle, developer_mode);
    }
    return rt64_3ds::create_render_context(rdram, handle, developer_mode);
}

// libctru splits the process's memory between the application heap and the
// linear heap at start (a weak function, replaced here). Its default gives
// the linear heap up to 32 MB and caps the application heap at 24 MB when
// memory is short: on an Old 3DS (64 MB) that leaves no room for the N64's
// RDRAM beside everything else. Here the linear heap (what does not fit in
// VRAM, the draw lists, audio buffers: under 8 MB in use in DK64) gets
// 32 MB when there is plenty, as before on a New 3DS, and 16 MB otherwise;
// the application heap gets the rest.
extern "C" char* fake_heap_start;
extern "C" char* fake_heap_end;
extern "C" u32 __ctru_heap, __ctru_heap_size, __ctru_linear_heap, __ctru_linear_heap_size;
extern "C" void __system_allocateHeaps(void) {
    Handle reslimit = 0;
    if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE))) svcBreak(USERBREAK_PANIC);
    s64 max_commit = 0, current_commit = 0;
    ResourceLimitType type = RESLIMIT_COMMIT;
    svcGetResourceLimitLimitValues(&max_commit, reslimit, &type, 1);
    svcGetResourceLimitCurrentValues(&current_commit, reslimit, &type, 1);
    svcCloseHandle(reslimit);
    const u32 remaining = (u32)(max_commit - current_commit) & ~0xFFFu;
    __ctru_linear_heap_size = remaining >= (96u << 20) ? (32u << 20) : (16u << 20);
    __ctru_heap_size = remaining - __ctru_linear_heap_size;
    if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0x0, __ctru_heap_size, MEMOP_ALLOC, (MemPerm)(MEMPERM_READ | MEMPERM_WRITE)))) svcBreak(USERBREAK_PANIC);
    if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR, (MemPerm)(MEMPERM_READ | MEMPERM_WRITE)))) svcBreak(USERBREAK_PANIC);
    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    fake_heap_start = (char*)__ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
}

void log_memory(const char* when) {
    // The memory the process got: libctru gives all of it, less the
    // executable, to the two heaps at start, so heap + linear heap + the
    // executable is the memory mode (64, 96, 124 MB...). The region's own
    // size says the same on the console (Azahar reports 96 MB there while
    // handing out 124 MB), and its free space reads 0 after the split.
    const struct mallinfo mi = mallinfo();
    fprintf(stderr, "recomp3ds: %s: heap %lu KB + linear heap %lu KB (%s; app region %lu KB); heap in use %lu KB (%lu KB taken), linear free %lu KB\n", when,
            (unsigned long)(envGetHeapSize() / 1024), (unsigned long)(envGetLinearHeapSize() / 1024),
            envIsHomebrew() ? ".3dsx" : "title",
            (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) / 1024),
            (unsigned long)(mi.uordblks / 1024), (unsigned long)(mi.arena / 1024),
            (unsigned long)(linearSpaceFree() / 1024));
}

// The CPU clock, from a loop of single-cycle ALU ops timed against the system
// tick (268 MHz whatever the CPU runs at). Best of three to ride out preemption.
u32 measure_cpu_mhz() {
    constexpr u32 kIters = 100000, kOpsPerIter = 64 + 2;
    u64 best = UINT64_MAX;
    for (int run = 0; run < 3; run++) {
        u32 n = kIters, a = 0;
        u64 t0 = svcGetSystemTick();
        __asm__ volatile(
            "1:\n"
            ".rept 64\n"
            "add %1, %1, #1\n"
            ".endr\n"
            "subs %0, %0, #1\n"
            "bne 1b\n"
            : "+r"(n), "+r"(a) : : "cc");
        u64 ticks = svcGetSystemTick() - t0;
        if (ticks < best) best = ticks;
    }
    return (u32)((u64)kIters * kOpsPerIter * (SYSCLOCK_ARM11 / 1000) / best / 1000);
}

// Average latency of a dependent load walk over 1 MB: slow from FCRAM, far
// quicker when the New 3DS L2 cache (2 MB) is on.
u32 measure_load_ns() {
    constexpr u32 kNodes = (1024 * 1024) / 32, kHops = 100000;
    u32* buf = static_cast<u32*>(malloc(kNodes * 32));
    u32* order = static_cast<u32*>(malloc(kNodes * sizeof(u32)));
    if (buf == nullptr || order == nullptr) { free(buf); free(order); return 0; }
    // One random cycle through all nodes, 32 bytes (one cache line) apart.
    for (u32 i = 0; i < kNodes; i++) order[i] = i;
    u32 seed = 12345;
    for (u32 i = kNodes - 1; i > 0; i--) {
        seed = seed * 1103515245u + 12345u;
        u32 j = (seed >> 8) % (i + 1);
        u32 t = order[i]; order[i] = order[j]; order[j] = t;
    }
    for (u32 i = 0; i < kNodes; i++) {
        buf[order[i] * 8] = order[(i + 1) % kNodes] * 8;
    }
    free(order);
    u32 p = 0;
    for (u32 i = 0; i < kNodes; i++) p = buf[p];    // warm up
    u64 t0 = svcGetSystemTick();
    for (u32 i = 0; i < kHops; i++) p = buf[p];
    u64 ticks = svcGetSystemTick() - t0;
    volatile u32 sink = p; (void)sink;
    free(buf);
    return (u32)(ticks * 1000000000ull / SYSCLOCK_ARM11 / kHops);
}

u32 log_cpu(const char* when) {
    u32 mhz = measure_cpu_mhz();
    // loadmon's calibrations hold at the clock they were taken at: the
    // two clocks are 268 and 804 MHz (exactly 1:3).
    recomp3ds::loadmon_set_mhz(mhz > 500 ? 804 : 268);
    fprintf(stderr, "recomp3ds: %s: CPU ~%lu MHz, 1 MB load walk %lu ns/load\n", when,
            (unsigned long)mhz, (unsigned long)measure_load_ns());
    return mhz;
}

// The New 3DS clock and L2 cache (settings menu: CPU speed). Off, a New 3DS
// runs at an Old 3DS's 268 MHz without L2 - the CPU side of an Old 3DS,
// though it keeps the fourth core and the memory.
bool g_speedup = true;
bool g_clock_booted = false;

}   // namespace

void recomp3ds::set_cpu_speed(bool new3ds) {
    g_speedup = new3ds;
    if (!g_clock_booted) return;       // run() applies the saved choice at boot
    osSetSpeedupEnable(new3ds);
    log_cpu(new3ds ? "CPU speed set to New 3DS" : "CPU speed set to Old 3DS");
}

const char* recomp3ds::base_path() { return g_base_path; }

void recomp3ds::request_exit() { g_exit_requested = true; }

int recomp3ds::run(const GameDesc& desc) {
    g_desc = &desc;

    gfxInitDefault();
    consoleInit(GFX_BOTTOM, nullptr);          // status text on the touch screen (screen_printf)
    recomp3ds::screen_init();
    if (__3dslink_host.s_addr != 0) {
        link3dsStdio();
    }
    snprintf(g_base_path, sizeof(g_base_path), "sdmc:/3ds/%s", desc.sd_dir);
    mkdir(g_base_path, 0777);
    recomp3ds::log_init(g_base_path);          // stderr: debug log + <base>/log.txt
    recomp3ds::autotest_load(g_base_path);
    {
        char path[192];
        snprintf(path, sizeof(path), "%s/SLOW.TXT", g_base_path);
        if (FILE* f = fopen(path, "r")) {
            int gfx_ms = 0, audio_ms = 0;
            if (fscanf(f, "%d %d", &gfx_ms, &audio_ms) >= 1) {
                g_slow_audio_ms = audio_ms;
                rt64_3ds::set_debug_gfx_delay_ms(gfx_ms);
                fprintf(stderr, "recomp3ds: SLOW.TXT: +%d ms per display list, +%d ms per audio task\n", gfx_ms, audio_ms);
            }
            fclose(f);
        }
    }

    bool is_new_3ds = false;
    APT_CheckNew3DS(&is_new_3ds);
    fprintf(stderr, "recomp3ds: %s starting on %s 3DS\n", desc.render.game_name, is_new_3ds ? "a New" : "an Old");
    recomp3ds::screen_printf("%s\n", desc.render.game_name);
    log_memory("boot");
    // Before the settings: a loaded setting's on_change may pick another map.
    recomp3ds::input_set_map(desc.button_map, desc.button_map_count);
    recomp3ds::input_set_controller_pak(desc.controller_pak);
    recomp3ds::settings_menu_init(g_base_path, desc);
    {
        // The ROM in memory, or read from the SD card as the game needs it
        // (settings.ini rom_stream: 0 auto, 1 in memory, 2 from the SD
        // card). Auto reads it from the card on an Old 3DS, or when the heap
        // could not hold it beside RDRAM and some room for the rest.
        const int mode = recomp3ds::settings_rom_stream();
        char rom_path[192];
        snprintf(rom_path, sizeof(rom_path), "%s/%s.z64", g_base_path, (const char*)desc.game_id.c_str());
        struct stat rom_st;
        const uint64_t rom_bytes = stat(rom_path, &rom_st) == 0 ? (uint64_t)rom_st.st_size : (32u << 20);
        constexpr uint64_t kRoomForTheRest = 24u << 20;
        const bool heap_short = envGetHeapSize() < rom_bytes + recomp::mem_size + kRoomForTheRest;
        const bool stream = mode == 2 || (mode == 0 && (!is_new_3ds || heap_short));
        if (stream) recomp3ds::rom_stream_install(2u << 20);
        fprintf(stderr, "recomp3ds: ROM %s (rom_stream %d%s)\n", stream ? "read from the SD card as needed" : "held in memory", mode,
                mode != 0 ? "" : !is_new_3ds ? ", auto: an Old 3DS" : heap_short ? ", auto: the heap is short" : ", auto");
    }
    u32 cpu_mhz = log_cpu("boot");
    g_clock_booted = true;
    if (is_new_3ds && g_speedup) {
        // The exheader asks for 804 MHz and the L2 cache, but on hardware the
        // CIA still starts at 268 MHz without L2 (measured: 271 MHz, 230 ns
        // loads; 816 MHz, 33 ns after this). libctru re-applies it after
        // every HOME Menu visit.
        osSetSpeedupEnable(true);
        cpu_mhz = log_cpu("after speedup request");
    }
    {
        char sub[160];
        snprintf(sub, sizeof(sub), "%s/saves", g_base_path);
        mkdir(sub, 0777);
    }
    recomp::register_config_path(std::filesystem::path(g_base_path));

    recomp3ds::audio_set_margin_frames(desc.audio_margin_frames);
    recomp3ds::audio_init();
    rt64_3ds::set_render_desc(desc.render);
    rt64_3ds::set_data_dir(g_base_path);   // the renderer's debug switches and captures

    // The runtime looks for <config>/<game_id>.z64; put the ROM there by that name.
    // The game's init runs after the runtime's last mod scan and just before
    // the mods load: the built-in mods' saved state goes in there first.
    {
        recomp::GameEntry entry = desc.entry;
        g_game_on_init = entry.on_init_callback;
        entry.on_init_callback = [](uint8_t* rdram, recomp_context* ctx) {
            g_rdram = rdram;
            recomp3ds::settings_apply_mods();
            if (g_game_on_init != nullptr) g_game_on_init(rdram, ctx);
        };
        recomp::register_game(entry);
    }
    if (desc.register_overlays) {
        desc.register_overlays();
    }
    if (desc.register_patches) {
        desc.register_patches();
    }
    // Hashes <config>/<game_id>.z64 and marks the game valid; is_rom_valid
    // only reads that set. (A ROM with the wrong hash is deleted by this.)
    recomp::check_all_stored_roms();

    ultramodern::renderer::callbacks_t renderer_callbacks{};
    renderer_callbacks.create_render_context = make_render_context;

    ultramodern::audio_callbacks_t audio_callbacks{};
    audio_callbacks.queue_samples = recomp3ds::audio_queue_samples;
    audio_callbacks.get_frames_remaining = recomp3ds::audio_frames_remaining;
    audio_callbacks.set_frequency = recomp3ds::audio_set_frequency;

    ultramodern::input::callbacks_t input_callbacks{};
    input_callbacks.poll_input = recomp3ds::input_poll;
    input_callbacks.get_input = recomp3ds::input_get;
    input_callbacks.set_rumble = recomp3ds::input_set_rumble;
    input_callbacks.get_connected_device_info = recomp3ds::input_device_info;

    ultramodern::gfx_callbacks_t gfx_callbacks{};
    gfx_callbacks.create_gfx = create_gfx;
    gfx_callbacks.create_window = create_window;
    gfx_callbacks.update_gfx = update_gfx;

    ultramodern::events::callbacks_t events_callbacks{};
    ultramodern::error_handling::callbacks_t error_callbacks{};
    error_callbacks.message_box = message_box;
    ultramodern::threads::callbacks_t threads_callbacks{};
    threads_callbacks.get_game_thread_name = game_thread_name;

    recomp::Configuration cfg{};
    cfg.project_version = desc.project_version;
    cfg.rsp_callbacks = desc.rsp;
    // Time the audio microcode: the game's callback hands back the ucode
    // function and this trampoline runs it under the tick counter.
    g_game_get_ucode = desc.rsp.get_rsp_microcode;
    g_audio_hle = desc.audio_hle;
    ultramodern::set_async_audio_tasks(desc.audio_async);
    g_audio_hle_family = desc.audio_hle_family;
    recomp3ds::naudio_hle_set_variant(desc.audio_hle_variant);
    if (g_game_get_ucode != nullptr) {
        cfg.rsp_callbacks.get_rsp_microcode = timed_get_ucode;
    }
    cfg.renderer_callbacks = renderer_callbacks;
    cfg.audio_callbacks = audio_callbacks;
    cfg.input_callbacks = input_callbacks;
    cfg.gfx_callbacks = gfx_callbacks;
    cfg.events_callbacks = events_callbacks;
    cfg.error_handling_callbacks = error_callbacks;
    cfg.threads_callbacks = threads_callbacks;
    cfg.message_queue_control.requeue_timer = false;

    if (!recomp::is_rom_valid(desc.game_id)) {
        recomp3ds::screen_printf("\nROM not found or wrong hash.\nPut it at %s/%s.z64\n\nPress START to exit.\n",
               g_base_path, (const char*)desc.game_id.c_str());
        fprintf(stderr, "recomp3ds: no valid ROM at %s/%s.z64\n", g_base_path, (const char*)desc.game_id.c_str());
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
        }
        gfxExit();
        return 1;
    }

    // The audio microcode interpreter shares core 2 with the renderer and
    // outranks it there, so every audio task stalls a frame. Core 1 (the
    // system core, time-limited for applications) takes it when available.
    // AUDIO_CORE.TXT holds 1 or 2 to force a core.
    // The game's N64 threads go to core 1 and the renderer, the audio task
    // and the DSP synthesizer to core 0 on an Old 3DS, which has no core 2:
    // the renderer is the larger load, and core 0 is the core the system
    // does not share. Goemon at the Old clock with two cores drew 17.5
    // frames a second with the renderer squeezed into core 1's share and
    // 24.4 swapped (three cores: 26.9; New 3DS, CPU speed Old 3DS,
    // 2026-10-03). The Cores row (settings.ini cores, read at start) picks
    // that layout on a New 3DS too, to test an Old 3DS on one.
    {
        (void)cpu_mhz;
        int want = 1;
        char path[192];
        snprintf(path, sizeof(path), "%s/AUDIO_CORE.TXT", g_base_path);
        if (FILE* f = fopen(path, "r")) { if (fscanf(f, "%d", &want) != 1) want = 1; fclose(f); }
        const int cores_row = recomp3ds::settings_cores();
        const bool two_cores = !is_new_3ds || cores_row != 0;
        const bool core2 = !two_cores;
        // With two cores, which one the game gets (GameDesc::two_cores_game_on_core0).
        const bool game_on_0 = cores_row == 2 || (cores_row == 0 && desc.two_cores_game_on_core0);
        g_sp_core = core2 ? 2 : 0;
        g_gfx_core = core2 ? 2 : 0;
        g_watch_cores = core2 ? 0x5 : 0x1;
        if (want == 1 || two_cores) {
            // PM refuses a limit above the exheader's MaxCpu (low 7 bits):
            // take the highest it accepts. More than 80 is accepted and
            // ignored (a New 3DS, as a CIA and as a .3dsx).
            u32 limit = 0;
            Result rc = -1;
            static const u32 kTry[] = { 80, 70, 60, 50, 40, 30, 25, 20 };
            for (u32 pct : kTry) {
                rc = APT_SetAppCpuTimeLimit(pct);
                if (R_SUCCEEDED(rc)) break;
            }
            APT_GetAppCpuTimeLimit(&limit);
            Thread probe = R_SUCCEEDED(rc) ? threadCreate([](void*) {}, nullptr, 4096, 0x30, 1, false) : nullptr;
            if (probe != nullptr) {
                threadJoin(probe, UINT64_MAX);
                threadFree(probe);
                g_core1_granted = true;
                g_watch_cores |= 0x2;
                if (core2) g_sp_core = 1;               // the audio task beside the renderer's core
                else if (game_on_0) g_gfx_core = g_sp_core = 1;   // two cores: the rest on core 1
                else g_game_core = 1;                   // two cores: the game on core 1
            }
            // Without time on core 1 everything shares core 0.
            g_helper_core = (core2 || game_on_0) && g_core1_granted ? 1 : 0;
            fprintf(stderr, "recomp3ds: core 1 time limit %lu%% (rc %08lx); %s: game on core %d, audio task on core %d, renderer on core %d, synth on core %d\n",
                    (unsigned long)limit, (unsigned long)rc, core2 ? "three cores" : "two cores",
                    g_game_core, g_sp_core, g_gfx_core, g_helper_core);
        }
        else {
            g_helper_core = 0;
            fprintf(stderr, "recomp3ds: audio task on core 2\n");
        }
        if (is_new_3ds) {
            // Core 3 is the system's (head tracking and other services): an
            // application's threads are refused there. Logged as evidence.
            // A .3dsx from the Homebrew Launcher was allowed (2026-10-03):
            // loadmon then watches it too, its calibration showing how much
            // of it the system leaves (against core 0's).
            Thread probe3 = threadCreate([](void*) {}, nullptr, 4096, 0x30, 3, false);
            fprintf(stderr, "recomp3ds: a thread on core 3: %s\n", probe3 != nullptr ? "created" : "refused");
            if (probe3 != nullptr) { threadJoin(probe3, UINT64_MAX); threadFree(probe3); g_core3 = true; g_watch_cores |= 0x8; }
        }
        // The renderer's own core-1 helper (the in-between builder) is a
        // New 3DS's extra: none when core 2 is left out.
        rt64_3ds::settings().no_core2 = two_cores;
        // GameDesc::two_cores_read_on_game_core: the lists' read on core 0
        // with the game, the renderer on core 1.
        rt64_3ds::settings().read_core = (two_cores && desc.two_cores_read_on_game_core && g_game_core == 0 && g_gfx_core == 1) ? 0 : -1;
        // Just below the game's threads (host_thread_spec): they go first.
        rt64_3ds::settings().read_priority = 0x31;
        ultramodern::set_host_thread_spec_callback(host_thread_spec);
    }
    // The DSP synthesizer's decoding thread wants core 1, granted just above.
    if (desc.synth.flavor != recomp3ds::SynthFlavor::None) recomp3ds::dsp_synth_init(desc.synth);
    {
        // Debug: the FPU control state threads start with (rounding mode in
        // bits 22-23, flush-to-zero in bit 24, default NaN in bit 25).
        u32 main_fpscr; __asm__ volatile("vmrs %0, fpscr" : "=r"(main_fpscr));
        static volatile u32 thread_fpscr = 0;
        Thread t = threadCreate([](void*) { u32 v; __asm__ volatile("vmrs %0, fpscr" : "=r"(v)); thread_fpscr = v; }, nullptr, 4096, 0x30, 0, false);
        if (t) { threadJoin(t, UINT64_MAX); threadFree(t); }
        fprintf(stderr, "recomp3ds: FPSCR main %08lx, new thread %08lx\n", (unsigned long)main_fpscr, (unsigned long)thread_fpscr);
    }
    // The game or one of its game modes (with the mod system).
    recomp3ds::game_modes_init(g_base_path, desc);
    const std::string game_mode = recomp3ds::pick_game_mode(desc);
    recomp3ds::game_mode_begin(desc, game_mode);
    if (!game_mode.empty() && recomp::get_rom_stream() == nullptr) {
        // The mode's ROM is streamed from the card; the game's needs no room either.
        recomp3ds::rom_stream_install(2u << 20);
    }
    recomp::start_game(desc.game_id, game_mode);
    recomp3ds::loadmon_start(g_watch_cores);
    recomp3ds::stall_watch_start(&g_rdram, &desc);
    log_memory("before start");
    recomp3ds::log_start_writer();
    recomp::start(cfg);        // returns when the game has quit
    log_memory("after exit");
    finish_exit();
}
