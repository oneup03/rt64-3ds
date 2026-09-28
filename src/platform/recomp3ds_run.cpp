// The 3DS entry point: console services, the runtime's callbacks, and the
// runtime itself. The game supplies a GameDesc; everything else is shared.
#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/stat.h>

#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/error_handling.hpp"
#include "ultramodern/events.hpp"
#include "ultramodern/host_thread.hpp"
#include "recomp3ds.h"
#include "recomp3ds_internal.h"
#include "naudio_hle.h"

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

uint8_t* g_rdram = nullptr;
// SLOW.TXT "<gfx ms> <audio ms>": pad every display list and audio task by
// that much, to reproduce console timing in the emulator.
int g_slow_audio_ms = -1;

RspExitReason timed_ucode(uint8_t* rdram, uint32_t ucode_addr) {
    g_rdram = rdram;
    u64 t0 = svcGetSystemTick();
    RspExitReason r = g_ucode_inner(rdram, ucode_addr);
    if (g_slow_audio_ms > 0) svcSleepThread((s64)g_slow_audio_ms * 1000000);
    g_ucode_ticks += svcGetSystemTick() - t0;
    g_ucode_tasks++;
    return r;
}

RspUcodeFunc* timed_get_ucode(const OSTask* task) {
    RspUcodeFunc* f = g_game_get_ucode(task);
    if (f == nullptr) {
        return nullptr;
    }
    if (g_audio_hle) {
        // The CPU interpreter replaces the game's (recompiled) microcode. In
        // differential mode it still runs the original to check itself.
        static RspUcodeFunc* game_ucode = nullptr;
        if (game_ucode == nullptr) {
            game_ucode = f;
            char path[192];
            snprintf(path, sizeof(path), "%s/AUDIO_DIFF.TXT", g_base_path);
            snprintf(path, sizeof(path), "%s/AUDIO_CAPTURE.TXT", g_base_path);
            if (FILE* c = fopen(path, "r")) {
                fclose(c);
                recomp3ds_naudio_capture_dir(g_base_path);
            }
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
                    recomp3ds::naudio_hle_set_reference(f);
                    fprintf(stderr, "recomp3ds: audio HLE in differential mode (AUDIO_DIFF.TXT present)\n");
                }
                else {
                    fprintf(stderr, "recomp3ds: audio task interpreted on the CPU (AUDIO_RSP.TXT selects the recompiled microcode)\n");
                }
            }
        }
        if (g_audio_hle && f == game_ucode) {
            f = recomp3ds::naudio_hle_run;
        }
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

// Thread placement: game code on core 0, renderer on core 2, the audio task
// on g_sp_core (core 1 when the system grants time there), pacing threads
// above the game so their sleeps wake it.
ultramodern::HostThreadSpec host_thread_spec(ultramodern::HostThreadKind kind) {
    using K = ultramodern::HostThreadKind;
    switch (kind) {
        case K::GameStart:
        case K::Game:    return { 128 * 1024, 0x30, 0 };
        case K::Gfx:     return { 256 * 1024, 0x2C, 2 };
        case K::SpTask:  return { 64 * 1024, 0x24, g_sp_core };
        case K::Vi:
        case K::Timer:   return { 32 * 1024, 0x28, 0 };
        default:         return { 32 * 1024, 0x30, 0 };
    }
}
volatile bool g_exit_requested = false;

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
    recomp3ds::loadmon_stop();
    recomp3ds::audio_shutdown();
    gfxExit();
    __appExit();
    if (__system_retAddr) __system_retAddr();
    svcExitProcess();
}

// Called by the runtime's main loop (which sleeps 1 ms between calls) on
// core 0, the game's core: it sleeps most of a VI itself so the main thread
// wakes ~60 times a second instead of 1000.
void update_gfx_inner();
void update_gfx(void*) {
    update_gfx_inner();
    if (!g_exit_requested) svcSleepThread(15 * 1000000ll);
}

void update_gfx_inner() {
    // Once a second: frame rate and CPU load to the log and the touch screen.
    static u64 last_report = 0;
    u64 now = svcGetSystemTick();
    if (last_report == 0) {
        last_report = now;
    }
    else if (now - last_report >= SYSCLOCK_ARM11) {
        last_report = now;
        int busy0 = -1, busy2 = -1;
        recomp3ds::loadmon_sample(&busy0, &busy2);
        const rt64_3ds::FrameStats& st = rt64_3ds::stats();
        // Audio microcode cost over the last second: ms of CPU and task count.
        unsigned ucode_ms = (unsigned)(g_ucode_ticks * 1000 / SYSCLOCK_ARM11);
        unsigned ucode_tasks = g_ucode_tasks;
        g_ucode_ticks = 0;
        g_ucode_tasks = 0;
        const recomp3ds::NaudioHleStats& hs = recomp3ds::naudio_hle_stats();
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
        fprintf(stderr, "stats: %d dl/s core0 %d%% core2 %d%% ucode %u ms/s in %u tasks audio %zu frames queued hle %u/%u diff-mismatch %u/%u unknown %u\n",
                st.dl_per_sec, busy0, busy2, ucode_ms, ucode_tasks, recomp3ds::audio_frames_remaining(),
                hs.tasks, hs.commands, hs.diff_mismatches, hs.diff_tasks, hs.unknown_opcodes);
        printf("\x1b[2;0H%3d fps  cpu0 %3d%%  cpu2 %3d%%  ucode %3u ms/s   \n", st.dl_per_sec, busy0, busy2, ucode_ms);
    }
    // Watchdog: a frame that stays in the replay or the GPU wait for 4 s is
    // a hung GPU; say which draw it was on, once.
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
    if (!aptMainLoop() || g_exit_requested) {
        static bool quitting = false;
        if (!quitting) {
            quitting = true;
            fprintf(stderr, "recomp3ds: quitting\n");
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

void message_box(const char* msg) {
    fprintf(stderr, "[message] %s\n", msg);
    printf("\n%s\n", msg);
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

void log_memory(const char* when) {
    fprintf(stderr, "recomp3ds: %s: app region free %lu KB, linear free %lu KB\n", when,
            (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) / 1024),
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

void log_cpu(const char* when) {
    u32 mhz = measure_cpu_mhz();
    fprintf(stderr, "recomp3ds: %s: CPU ~%lu MHz, 1 MB load walk %lu ns/load\n", when,
            (unsigned long)mhz, (unsigned long)measure_load_ns());
}

}   // namespace

const char* recomp3ds::base_path() { return g_base_path; }

void recomp3ds::request_exit() { g_exit_requested = true; }

int recomp3ds::run(const GameDesc& desc) {
    g_desc = &desc;

    gfxInitDefault();
    consoleInit(GFX_BOTTOM, nullptr);          // stdout: status text on the touch screen
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
    printf("%s\n", desc.render.game_name);
    log_memory("boot");
    log_cpu("boot");
    if (is_new_3ds) {
        // An installed title gets 804 MHz and the L2 cache from its exheader;
        // ask PTM as well in case the launcher did not apply them.
        osSetSpeedupEnable(true);
        log_cpu("after speedup request");
    }
    {
        char sub[160];
        snprintf(sub, sizeof(sub), "%s/saves", g_base_path);
        mkdir(sub, 0777);
    }
    recomp::register_config_path(std::filesystem::path(g_base_path));

    recomp3ds::audio_init();
    rt64_3ds::set_render_desc(desc.render);

    // The runtime looks for <config>/<game_id>.z64; put the ROM there by that name.
    recomp::register_game(desc.entry);
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
    cfg.rsp_callbacks = desc.rsp;
    // Time the audio microcode: the game's callback hands back the ucode
    // function and this trampoline runs it under the tick counter.
    g_game_get_ucode = desc.rsp.get_rsp_microcode;
    g_audio_hle = desc.audio_hle;
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
        printf("\nROM not found or wrong hash.\nPut it at %s/%s.z64\n\nPress START to exit.\n",
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
    {
        int want = 1;
        char path[192];
        snprintf(path, sizeof(path), "%s/AUDIO_CORE.TXT", g_base_path);
        if (FILE* f = fopen(path, "r")) { if (fscanf(f, "%d", &want) != 1) want = 1; fclose(f); }
        g_sp_core = 2;
        if (want == 1) {
            // PM refuses a limit above the exheader's MaxCpu (low 7 bits):
            // take the highest it accepts.
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
                g_sp_core = 1;
            }
            fprintf(stderr, "recomp3ds: core 1 time limit %lu%% (rc %08lx), audio task on core %d\n", (unsigned long)limit, (unsigned long)rc, g_sp_core);
        }
        ultramodern::set_host_thread_spec_callback(host_thread_spec);
    }
    {
        // Debug: the FPU control state threads start with (rounding mode in
        // bits 22-23, flush-to-zero in bit 24, default NaN in bit 25).
        u32 main_fpscr; __asm__ volatile("vmrs %0, fpscr" : "=r"(main_fpscr));
        static volatile u32 thread_fpscr = 0;
        Thread t = threadCreate([](void*) { u32 v; __asm__ volatile("vmrs %0, fpscr" : "=r"(v)); thread_fpscr = v; }, nullptr, 4096, 0x30, 0, false);
        if (t) { threadJoin(t, UINT64_MAX); threadFree(t); }
        fprintf(stderr, "recomp3ds: FPSCR main %08lx, new thread %08lx\n", (unsigned long)main_fpscr, (unsigned long)thread_fpscr);
    }
    recomp::start_game(desc.game_id, "");
    recomp3ds::loadmon_start(is_new_3ds);
    log_memory("before start");
    recomp::start(cfg);        // returns when the game has quit
    log_memory("after exit");
    finish_exit();
}
