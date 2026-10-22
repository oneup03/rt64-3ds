// A sampling profile of the game's recompiled code (game_profile.h).
#include "game_profile.h"
#include "recomp3ds_internal.h"

#include <3ds.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
const char* volatile recomp3ds_trace_func = nullptr;
}

namespace {

// Open addressing by the name's address: the games have about 4,500
// functions, a few hundred of them seen in a session.
constexpr int kSlots = 8192;
constexpr uint32_t kMille = 1000;       // a sample is split between idle and the function in thousandths
struct Slot {
    const char* name = nullptr;
    uint32_t interval = 0, total = 0;   // thousandths of a sample
};
Slot g_slots[kSlots];
uint32_t g_samples = 0, g_idle = 0;             // the interval's: samples taken, thousandths idle
uint32_t g_total_samples = 0, g_total_idle = 0;
uint32_t g_dropped = 0;                         // samples with no function yet (before the game's start)
int g_game_core = 0;
Thread g_thread = nullptr;
volatile bool g_run = false;
LightLock g_lock;

inline uint32_t slot_of(const char* p) {
    uint32_t v = (uint32_t)(uintptr_t)p;
    v ^= v >> 15;
    v *= 0x2C1B3C6Du;
    v ^= v >> 12;
    return v & (kSlots - 1);
}

void sampler(void*) {
    uint32_t last_idle = 0, per_second = 0;
    recomp3ds::loadmon_idle_counts(g_game_core, &last_idle, &per_second);
    while (g_run) {
        svcSleepThread(1000000ll);
        uint32_t idle = 0;
        recomp3ds::loadmon_idle_counts(g_game_core, &idle, &per_second);
        const char* f = recomp3ds_trace_func;
        // The idle thread's counts since the last sample against its rate
        // with the core to itself: the fraction of the millisecond the
        // game's core had nothing to run.
        uint32_t idle_mille = 0;
        if (per_second != 0) {
            const uint64_t m = (uint64_t)(idle - last_idle) * kMille * 1000ull / per_second;
            idle_mille = m > kMille ? kMille : (uint32_t)m;
        }
        last_idle = idle;
        LightLock_Lock(&g_lock);
        g_samples++;
        g_idle += idle_mille;
        if (f == nullptr) {
            g_dropped++;
        }
        else {
            uint32_t i = slot_of(f);
            for (int n = 0; n < kSlots; n++, i = (i + 1) & (kSlots - 1)) {
                if (g_slots[i].name == f || g_slots[i].name == nullptr) {
                    g_slots[i].name = f;
                    g_slots[i].interval += kMille - idle_mille;
                    break;
                }
            }
        }
        LightLock_Unlock(&g_lock);
    }
}

}   // namespace

void recomp3ds::game_profile_start(int game_core, int sampler_core) {
    if (g_thread != nullptr) return;
    LightLock_Init(&g_lock);
    g_game_core = game_core;
    g_run = true;
    // Above the renderer (0x2C) so each sample lands on time; a sleep of a
    // millisecond between them.
    g_thread = threadCreate(sampler, nullptr, 8 * 1024, 0x21, sampler_core, false);
    if (g_thread == nullptr) g_thread = threadCreate(sampler, nullptr, 8 * 1024, 0x21, -2, false);
    fprintf(stderr, "game-profile: sampling the game's functions 1000 times a second (game core %d, from core %d); the top ones every 10 s\n",
            game_core, sampler_core);
}

bool recomp3ds::game_profile_enabled() { return g_thread != nullptr; }

void recomp3ds::game_profile_report(bool final) {
    if (g_thread == nullptr) return;
    std::vector<std::pair<uint32_t, const char*>> top;
    LightLock_Lock(&g_lock);
    const uint32_t samples = final ? g_total_samples + g_samples : g_samples;
    const uint32_t idle = final ? g_total_idle + g_idle : g_idle;
    for (Slot& s : g_slots) {
        if (s.name == nullptr) continue;
        const uint32_t v = final ? s.total + s.interval : s.interval;
        if (v != 0) top.emplace_back(v, s.name);
        s.total += s.interval;
        s.interval = 0;
    }
    g_total_samples += g_samples;
    g_total_idle += g_idle;
    g_samples = 0;
    g_idle = 0;
    LightLock_Unlock(&g_lock);
    if (samples == 0) return;
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const double per_sample = 100.0 / ((double)samples * kMille);
    fprintf(stderr, "game-profile: %s %lu samples, the game's core idle %.1f%%; its time by function:\n", final ? "whole session," : "last",
            (unsigned long)samples, (double)idle * per_sample);
    // Eight to a line: the log cuts lines at 512 characters.
    const size_t count = std::min(top.size(), final ? (size_t)32 : (size_t)16);
    char line[480];
    int n = 0;
    for (size_t i = 0; i < count; i++) {
        n += snprintf(line + n, sizeof(line) - n, " %s %.1f%%", top[i].second, (double)top[i].first * per_sample);
        if (i % 8 == 7 || i + 1 == count) {
            fprintf(stderr, "game-profile:%s\n", line);
            n = 0;
        }
    }
}

void recomp3ds::game_profile_stop() {
    if (g_thread == nullptr) return;
    g_run = false;
    threadJoin(g_thread, 100 * 1000000ll);
    threadFree(g_thread);
    g_thread = nullptr;
}
