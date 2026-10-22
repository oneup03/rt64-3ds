// CPU load monitor: an idle thread at the lowest priority on each core that
// counts how often it gets the CPU. Compared with a calibration taken before
// the game starts, that gives the core's busy fraction without touching the
// runtime. The calibrations also compare the cores: one the system shares
// (core 1 under its time limit, a New 3DS's core 3) counts slower.
#include <3ds.h>
#include <cstdio>

#include "recomp3ds_internal.h"

namespace {

struct alignas(32) Monitor {   // one cache line each: each core writes its own
    Thread thread = nullptr;
    volatile u32 count = 0;
    u32 calibration = 0;      // counts per second with nothing else running
    u32 last = 0;
    int core = 0;
    u32 peek_last = 0;        // loadmon_peek's own (the stall watch's)
    u64 peek_tick = 0;
};

constexpr int kCores = 4;
Monitor g_mon[kCores];        // by core
volatile bool g_running = false;
// The CPU clock now and when the calibrations were taken (the idle loop's
// count scales with it: the CPU speed row switches 268 and 804 MHz).
u32 g_mhz = 268, g_cal_mhz = 268;

void idle_thread(void* arg) {
    Monitor* m = static_cast<Monitor*>(arg);
    while (g_running) {
        // A little work per iteration so the count is coarse enough to read.
        for (int i = 0; i < 64; i++) {
            __asm__ volatile("" ::: "memory");
        }
        m->count = m->count + 1;
    }
}

}   // namespace

namespace {

unsigned g_mask = 0;
// Thread slots loadmon_resume leaves free (an application has 32, and a
// game may still make one: a capture's writer, a game thread later on).
constexpr unsigned kThreadsSpare = 3;

void stop_threads() {
    g_running = false;
    for (int c = 0; c < kCores; c++) {
        if (g_mon[c].thread != nullptr) {
            if (R_SUCCEEDED(threadJoin(g_mon[c].thread, 200 * 1000000ull))) threadFree(g_mon[c].thread);
            else fprintf(stderr, "loadmon: core %d's idle thread did not finish (left to the exit)\n", c);
            g_mon[c].thread = nullptr;
        }
    }
}

}   // namespace

// The calibration, then the idle threads let go until loadmon_resume: an
// application gets 32 threads, and a New 3DS port with its renderer's
// helpers, the DSP synthesizer, the ROM stream's read-ahead and the
// settings writer comes near that before the game makes its own (Dinosaur
// Planet's game thread failed at boot on a console, 2026-10-18; Azahar sets
// no limit). Until then the core loads read "-".
void recomp3ds::loadmon_start(unsigned core_mask) {
    g_mask = core_mask;
    g_running = true;
    for (int c = 0; c < kCores; c++) {
        g_mon[c].core = c;
        g_mon[c].thread = (core_mask >> c) & 1 ? threadCreate(idle_thread, &g_mon[c], 4096, 0x3F, c, false) : nullptr;
    }
    // Calibrate: everything else is idle now.
    g_cal_mhz = g_mhz;
    svcSleepThread(200 * 1000 * 1000LL);
    char line[160];
    int used = snprintf(line, sizeof(line), "loadmon: calibration");
    for (int c = 0; c < kCores; c++) {
        if (g_mon[c].thread == nullptr) continue;
        g_mon[c].calibration = g_mon[c].count * 5;
        g_mon[c].count = 0;
        used += snprintf(line + used, sizeof(line) - used, " core%d %lu/s", c, (unsigned long)g_mon[c].calibration);
    }
    fprintf(stderr, "%s\n", line);
    stop_threads();
}

void recomp3ds::loadmon_resume() {
    unsigned used = 0, limit = 0;
    const bool known = recomp3ds::thread_budget(&used, &limit);
    g_running = true;
    int started = 0;
    for (int c = 0; c < kCores; c++) {
        if (!((g_mask >> c) & 1) || g_mon[c].calibration == 0) continue;
        if (known && used + 1 + kThreadsSpare > limit) break;
        g_mon[c].count = 0;
        g_mon[c].last = 0;
        g_mon[c].peek_tick = 0;
        g_mon[c].thread = threadCreate(idle_thread, &g_mon[c], 4096, 0x3F, c, false);
        if (g_mon[c].thread != nullptr) { started++; used++; }
    }
    fprintf(stderr, "loadmon: %d core monitors running (threads %u of %u in use)\n", started, used, limit);
}

// A core's idle count now and the idle thread's rate with the core to
// itself (counts a second at the current clock); both 0 for a core not
// watched. The game profile reads the count a thousand times a second.
void recomp3ds::loadmon_idle_counts(int core, uint32_t* count, uint32_t* per_second) {
    *count = 0;
    *per_second = 0;
    if (core < 0 || core >= kCores) return;
    const Monitor& m = g_mon[core];
    if (m.thread == nullptr || m.calibration == 0) return;
    *count = m.count;
    *per_second = (uint32_t)((u64)m.calibration * g_mhz / g_cal_mhz);
}

// Busy percentage of a core since its previous call (call once a second
// for each core), or -1 for a core not watched.
int recomp3ds::loadmon_sample(int core) {
    if (core < 0 || core >= kCores) return -1;
    Monitor& m = g_mon[core];
    if (m.thread == nullptr || m.calibration == 0) return -1;
    const u32 now = m.count;
    const u32 delta = now - m.last;
    m.last = now;
    const u64 calibration = (u64)m.calibration * g_mhz / g_cal_mhz;
    int idle = calibration != 0 ? (int)((u64)delta * 100 / calibration) : 100;
    if (idle > 100) idle = 100;
    return 100 - idle;
}

// Busy percentage of a core since this function's previous call for it,
// apart from loadmon_sample's (the stall watch, while the main thread that
// samples once a second may be the one stopped); -1 on the first call or a
// core not watched.
int recomp3ds::loadmon_peek(int core) {
    if (core < 0 || core >= kCores) return -1;
    Monitor& m = g_mon[core];
    if (m.thread == nullptr || m.calibration == 0) return -1;
    const u64 t = svcGetSystemTick();
    const u32 now = m.count;
    if (m.peek_tick == 0) { m.peek_tick = t; m.peek_last = now; return -1; }
    const u64 dt = t - m.peek_tick;
    const u32 delta = now - m.peek_last;
    m.peek_tick = t;
    m.peek_last = now;
    const double expected = (double)m.calibration * g_mhz / g_cal_mhz * (double)dt / (double)SYSCLOCK_ARM11;
    int idle = expected > 0.0 ? (int)((double)delta * 100.0 / expected) : 100;
    if (idle > 100) idle = 100;
    return 100 - idle;
}

void recomp3ds::loadmon_set_mhz(unsigned mhz) {
    if (mhz != 0) g_mhz = mhz;
}

// At exit. An idle thread runs at the lowest priority, so on a core a game
// thread is spinning on (the forced exit's case) it never sees g_running
// drop: a bounded wait, and a thread that did not finish is left to die
// with the process rather than joined forever.
void recomp3ds::loadmon_stop() {
    stop_threads();
}
