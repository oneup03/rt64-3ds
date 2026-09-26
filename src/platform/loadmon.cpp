// CPU load monitor: an idle thread at the lowest priority on each core that
// counts how often it gets the CPU. Compared with a calibration taken before
// the game starts, that gives the core's busy fraction without touching the
// runtime. Core 0 is the game; core 2 (New 3DS) the renderer and audio.
#include <3ds.h>
#include <cstdio>

#include "recomp3ds_internal.h"

namespace {

struct Monitor {
    Thread thread = nullptr;
    volatile u32 count = 0;
    u32 calibration = 0;      // counts per second with nothing else running
    u32 last = 0;
    int core = 0;
};

Monitor g_mon[2];
volatile bool g_running = false;

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

void recomp3ds::loadmon_start(bool has_core2) {
    g_running = true;
    g_mon[0].core = 0;
    g_mon[1].core = 2;
    int n = has_core2 ? 2 : 1;
    for (int i = 0; i < n; i++) {
        g_mon[i].thread = threadCreate(idle_thread, &g_mon[i], 4096, 0x3F, g_mon[i].core, false);
        if (g_mon[i].thread == nullptr && g_mon[i].core != 0) {
            g_mon[i].thread = nullptr;
        }
    }
    // Calibrate: everything else is idle now.
    svcSleepThread(200 * 1000 * 1000LL);
    for (int i = 0; i < n; i++) {
        g_mon[i].calibration = g_mon[i].count * 5;
        g_mon[i].count = 0;
    }
    fprintf(stderr, "loadmon: calibration core0 %lu/s core2 %lu/s\n",
            (unsigned long)g_mon[0].calibration, (unsigned long)g_mon[1].calibration);
}

// Busy percentage of each core since the previous call (call once a second).
void recomp3ds::loadmon_sample(int* busy0, int* busy2) {
    for (int i = 0; i < 2; i++) {
        int* out = i == 0 ? busy0 : busy2;
        if (g_mon[i].thread == nullptr || g_mon[i].calibration == 0) {
            *out = -1;
            continue;
        }
        u32 now = g_mon[i].count;
        u32 delta = now - g_mon[i].last;
        g_mon[i].last = now;
        int idle = (int)((u64)delta * 100 / g_mon[i].calibration);
        if (idle > 100) idle = 100;
        *out = 100 - idle;
    }
}

void recomp3ds::loadmon_stop() {
    g_running = false;
    for (int i = 0; i < 2; i++) {
        if (g_mon[i].thread != nullptr) {
            threadJoin(g_mon[i].thread, UINT64_MAX);
            threadFree(g_mon[i].thread);
            g_mon[i].thread = nullptr;
        }
    }
}
