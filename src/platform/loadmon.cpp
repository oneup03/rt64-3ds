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

void recomp3ds::loadmon_start(unsigned core_mask) {
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

void recomp3ds::loadmon_stop() {
    g_running = false;
    for (int c = 0; c < kCores; c++) {
        if (g_mon[c].thread != nullptr) {
            threadJoin(g_mon[c].thread, UINT64_MAX);
            threadFree(g_mon[c].thread);
            g_mon[c].thread = nullptr;
        }
    }
}
