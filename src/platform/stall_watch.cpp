// Core 0 stall watch. The game's threads and the main thread (which runs
// the per-second report, the pad, the HOME button) share core 0 and its
// priority, and the 3DS does not time-slice equal priorities; the runtime
// switches game threads only when one sends or receives a message. A game
// thread that computes without one (a loop polling memory another thread
// would change, a long load) holds core 0 for as long as it runs, and the
// main thread with it: the report stops, HOME does nothing, and the log
// just ends. This thread, above them, notices the main thread's silence
// and says which game thread has been running, and for how long.
// It also puts a red line on the touch screen, for a photo when the log
// can't say more, while one of these lasts: the main thread stopped or no
// frame drawn for 3 s (15 s while the game has the video blanked: DK64 draws
// nothing through its loads, 4-7 s on an Old 3DS, which lit the row at every
// load), the log's card write not returning for 15 s (a stuck
// card stops the log, and a game reading its ROM from the card, without a
// word; a slow card takes over 3 s for some writes once the log has grown a
// few MB, and the game runs on meanwhile). When the
// screen and that line freeze too, the whole console stopped.
#include <3ds.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" const char* recomp3ds_game_stall_info() __attribute__((weak));

#include "recomp3ds.h"
#include "recomp3ds_internal.h"
#include "ultramodern/ultramodern.hpp"
#include "rt64_3ds.h"

// The runtime's diagnostic counts (nmr patch 0030; weak so a runtime
// without it still links): 0 external messages queued, 1 not sent (queue full), 2 game
// thread resumes, 3 timer thread passes, 4 VI thread passes, 5 idle waits,
// 6 blocking receives that waited, 7 blocking sends that waited.
extern "C" __attribute__((weak)) volatile uint32_t ultramodern_dbg_counts[8];

namespace {

std::atomic<uint32_t> g_beat{ 0 };
Thread g_thread = nullptr;
volatile bool g_run = false;
std::atomic<bool> g_paused{ false };     // stall_watch_pause: the HOME prompt holds the game
uint8_t* const* g_watch_rdram = nullptr;     // the host's pointer to RDRAM (set once the game runs)
const recomp3ds::GameDesc* g_watch_desc = nullptr;

std::string thread_name(PTR(OSThread) thread) {
    if (*g_watch_rdram != nullptr && g_watch_desc != nullptr && g_watch_desc->get_game_thread_name != nullptr) {
        std::string name = g_watch_desc->get_game_thread_name(reinterpret_cast<const OSThread*>(*g_watch_rdram + (thread & 0x7FFFFFFFu)));
        if (name.rfind("[Game] ", 0) == 0) name.erase(0, 7);
        return name;
    }
    return std::to_string(thread);
}

// How long a game thread has been running. UltraThreadContext::load_since
// holds only the tick's low 32 bits, which come round every 16 s, so a run
// is timed from the first pass that saw it (they come every 250 ms).
u64 running_ticks(PTR(OSThread) thread, uint32_t load_since) {
    struct Seen { PTR(OSThread) thread; uint32_t since; u64 start; };
    static Seen seen[4] = {};
    static int next = 0;
    const u64 now = svcGetSystemTick();
    for (const Seen& s : seen) {
        if (s.thread == thread && s.since == load_since) return now - s.start;
    }
    Seen& s = seen[next];
    next = (next + 1) % 4;
    s = { thread, load_since, now - (uint32_t)((uint32_t)now - load_since) };
    return now - s.start;
}

// Which game thread is running and since when (UltraThreadContext::
// load_since: the tick it last started running, 0 while it waits), and the
// others' wake-ups since the stall began.
void report(u64 stalled, const ultramodern::ThreadLoad* before, size_t before_n) {
    const float ms = 1000.0f / (float)SYSCLOCK_ARM11;
    std::string running, others;
    if (*g_watch_rdram != nullptr) {
        ultramodern::ThreadLoad loads[32];
        const size_t n = ultramodern::get_thread_loads(loads, 32);
        for (size_t i = 0; i < n; i++) {
            const OSThread* t = reinterpret_cast<const OSThread*>(*g_watch_rdram + (loads[i].thread & 0x7FFFFFFFu));
            const UltraThreadContext* c = t->context;
            uint32_t wakes = loads[i].wakeups;
            for (size_t p = 0; p < before_n; p++) {
                if (before[p].thread == loads[i].thread) { wakes -= before[p].wakeups; break; }
            }
            char item[96];
            if (c != nullptr && c->load_since != 0) {
                snprintf(item, sizeof(item), "%s%s (priority %d) for %.0f ms", running.empty() ? "" : ", ", thread_name(loads[i].thread).c_str(),
                         (int)t->priority, (float)running_ticks(loads[i].thread, c->load_since) * ms);
                running += item;
            }
            else {
                snprintf(item, sizeof(item), "%s%s %u", others.empty() ? "" : ", ", thread_name(loads[i].thread).c_str(), (unsigned)wakes);
                others += item;
            }
        }
    }
    // A game may name what its threads were running (a diagnostic build's
    // function trace): recomp3ds_game_stall_info, weak.
    if (recomp3ds_game_stall_info != nullptr) {
        const char* info = recomp3ds_game_stall_info();
        if (info != nullptr) fprintf(stderr, "recomp3ds: STALL: the game's last functions: %s\n", info);
    }
    fprintf(stderr, "recomp3ds: STALL: the main thread has not run for %.1f s; game thread running: %s; wake-ups since of the others: %s\n",
            (float)stalled * ms / 1000.0f, running.empty() ? "none" : running.c_str(), others.c_str());
    rt64_3ds::dump_progress();
}

// The game thread the runtime has running and for how long ("" none),
// with "+N" when N more threads also count as running (only one should).
void running_game_thread(char* out, size_t size) {
    out[0] = '\0';
    if (*g_watch_rdram == nullptr) return;
    ultramodern::ThreadLoad loads[32];
    const size_t n = ultramodern::get_thread_loads(loads, 32);
    int more = -1;
    for (size_t i = 0; i < n; i++) {
        const OSThread* t = reinterpret_cast<const OSThread*>(*g_watch_rdram + (loads[i].thread & 0x7FFFFFFFu));
        const UltraThreadContext* c = t->context;
        if (c != nullptr && c->load_since != 0) {
            if (++more == 0) snprintf(out, size, "%s %.0fs", thread_name(loads[i].thread).c_str(), (float)running_ticks(loads[i].thread, c->load_since) / (float)SYSCLOCK_ARM11);
        }
    }
    if (more > 0) {
        const size_t used = strlen(out);
        snprintf(out + used, size - used, " +%d", more);
    }
}

// The touch screen's alert lines: what has stopped, for how long, and above
// it what was going on (the cores' load since the last check: near 100 with
// everything stopped is something spinning, near 0 everything waiting; where
// the main thread is; the renderer's phase; the game thread running).
// Cleared once nothing has stopped (only after an alert, and not while the
// HOME Menu or sleep has the screens: the main thread and the frames stop
// there by design).
void alert(u64 main_stalled, u64 frames_stalled, u64 frames_limit_s, uint32_t write_ms, int busy0, int busy1, int busy2, const char* rates) {
    static bool shown = false;
    const float s = 1.0f / (float)SYSCLOCK_ARM11;
    char line[64] = "";
    int n = 0;
    if (main_stalled > 3ull * SYSCLOCK_ARM11) n += snprintf(line + n, sizeof(line) - n, "main %.0fs ", (float)main_stalled * s);
    if (frames_stalled > frames_limit_s * SYSCLOCK_ARM11) n += snprintf(line + n, sizeof(line) - n, "frames %.0fs ", (float)frames_stalled * s);
    if (write_ms > 15000) n += snprintf(line + n, sizeof(line) - n, "card write %us ", (unsigned)(write_ms / 1000));
    if (n > 0) {
        char text[64];
        snprintf(text, sizeof(text), "STOPPED: %s", line);
        recomp3ds::screen_alert(text);
        char game[40];
        running_game_thread(game, sizeof(game));
        char detail[96];
        // Core loads as 0:N 1:N 2:N (-1 not watched), the main thread's stage,
        // the renderer's phase, the game thread running.
        snprintf(detail, sizeof(detail), "0:%d 1:%d 2:%d m@%s g%u %s", busy0, busy1, busy2, recomp3ds::main_stage_name(recomp3ds::main_stage()),
                 (unsigned)rt64_3ds::progress().phase, game[0] ? game : "-");
        recomp3ds::screen_alert(detail, 28);
        if (rates[0] != '\0') recomp3ds::screen_alert(rates, 27);
        if (!shown) {
            fprintf(stderr, "recomp3ds: STOPPED: %s(the touch screen's alert line); %s; %s\n", line, detail, rates);
            // What the renderer's threads are waiting for (frames stopped
            // with the main thread alive gets no STALL report).
            rt64_3ds::dump_progress();
            recomp3ds::log_sync();
        }
        shown = true;
    }
    else if (shown) {
        recomp3ds::screen_alert("");
        recomp3ds::screen_alert("", 28);
        recomp3ds::screen_alert("", 27);
        shown = false;
    }
}

void watch(void*) {
    uint32_t last = g_beat.load();
    u64 since = svcGetSystemTick();
    bool reported = false;
    ultramodern::ThreadLoad before[32];
    size_t before_n = 0;
    uint32_t last_frames = rt64_3ds::progress().frames;
    u64 frames_since = since;
    while (g_run) {
        svcSleepThread(250 * 1000000ll);
        const uint32_t beat = g_beat.load();
        const u64 now = svcGetSystemTick();
        const uint32_t frames = rt64_3ds::progress().frames;
        // A blanked video (a load) draws nothing by design: 15 s before the frames count as stopped.
        const bool blanked = rt64_3ds::progress().blanked != 0;
        // Not while the HOME prompt holds the game (stall_watch_pause) or the
        // HOME Menu or sleep has the application: the main thread and the
        // frames stop there by design.
        const bool quiet = g_paused.load(std::memory_order_relaxed) || !aptIsActive();
        if (frames != last_frames || frames == 0 || beat == 0 || quiet) { last_frames = frames; frames_since = now; }
        const int busy0 = recomp3ds::loadmon_peek(0), busy1 = recomp3ds::loadmon_peek(1), busy2 = recomp3ds::loadmon_peek(2);
        // The runtime's counts a second since the last pass: e queued
        // external messages, f not sent, r game thread resumes, t timer
        // passes, v VI passes, i idle waits, w receive waits, s send waits.
        char rates[48] = "";
        if (&ultramodern_dbg_counts[0] != nullptr) {
            static uint32_t prev[8];
            static u64 prev_t = 0;
            uint32_t cur[8];
            for (int i = 0; i < 8; i++) cur[i] = ultramodern_dbg_counts[i];
            if (prev_t != 0 && now > prev_t) {
                const float per_s = (float)SYSCLOCK_ARM11 / (float)(now - prev_t);
                auto r = [&](int i) { return (unsigned)((float)(cur[i] - prev[i]) * per_s + 0.5f); };
                snprintf(rates, sizeof(rates), "e%u f%u r%u t%u v%u i%u w%u s%u", r(0), r(1), r(2), r(3), r(4), r(5), r(6), r(7));
            }
            for (int i = 0; i < 8; i++) prev[i] = cur[i];
            prev_t = now;
        }
        if (beat != 0) alert(!quiet && beat == last ? now - since : 0, now - frames_since, blanked ? 15ull : 3ull, recomp3ds::log_write_stuck_ms(), busy0, busy1, busy2, rates);
        // Not before the main thread's loop has begun (the runtime's start,
        // the renderer's, run on it first).
        if (beat == 0) {
            since = now;
            continue;
        }
        if (beat != last || quiet) {
            if (reported) fprintf(stderr, "recomp3ds: STALL over: the main thread ran again after %.1f s\n", (float)(now - since) / (float)SYSCLOCK_ARM11);
            last = beat;
            since = now;
            reported = false;
            // The wake-up counts a stall's report compares with.
            before_n = *g_watch_rdram != nullptr ? ultramodern::get_thread_loads(before, 32) : 0;
            continue;
        }
        if (!reported && now - since > 3ull * SYSCLOCK_ARM11) {
            reported = true;
            report(now - since, before, before_n);
        }
    }
}

}   // namespace

void recomp3ds::stall_watch_beat() {
    g_beat.fetch_add(1, std::memory_order_relaxed);
}

void recomp3ds::stall_watch_start(uint8_t* const* rdram, const GameDesc* desc) {
    if (g_thread != nullptr) return;
    g_watch_rdram = rdram;
    g_watch_desc = desc;
    g_run = true;
    // Above the game threads and the main thread (0x30); the log writer
    // (0x27) is above them and the VI and timer threads, so the report
    // reaches the card.
    g_thread = threadCreate(watch, nullptr, 16 * 1024, 0x20, -2, false);
}

void recomp3ds::stall_watch_pause(bool paused) {
    g_paused.store(paused, std::memory_order_relaxed);
}

void recomp3ds::stall_watch_stop() {
    if (g_thread == nullptr) return;
    g_run = false;
    // A thread that did not finish is left to the exit, not freed under itself.
    if (R_SUCCEEDED(threadJoin(g_thread, 1000000000ull))) threadFree(g_thread);
    g_thread = nullptr;
}
