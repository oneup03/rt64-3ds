// The citro3d renderer: interprets each display list into a FrameRecord and
// replays it onto the top screen (mono for now; the stereo pair follows the
// same replay with per-draw shifts).
#include <3ds.h>
#include <citro3d.h>
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <ctime>
#include <unordered_map>
#include <vector>

#include "rt64_3ds.h"
#include "rt64_3ds_dl.h"
#include "rt64_3ds_record.h"
#include "rt64_3ds_texture.h"
#include "rt64_3ds_tev.h"
#include "rt64_3ds_stereo.h"
#include "rt64_3ds_shbin.h"
#include "ultramodern/extensions.h"

extern "C" void C3Di_UpdateContext(void);
// citro3d's own frame split (internal, exported): closes the commands
// recorded since the last split, with the framebuffer flush a draw needs.
extern "C" bool C3Di_SplitFrame(u32** pBuf, u32* pSize);

namespace {

constexpr uint32_t kVboVerts = 65536;

using GpuVertex = rt64_3ds::Vtx3ds;   // the record's vertex is the PICA's attribute layout

// What the CPU wrote for the GPU, out of the data cache: the kernel's own
// flush on the calling core (GSPGPU_FlushDataCache is a request to the GSP
// module on core 1, which waits for that core to be free).
inline void flush_dcache(const void* p, size_t bytes) {
    if (bytes != 0) svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)p, bytes);
}

#define g_stats rt64_3ds::mutable_stats()

// Frame read-back (rt64_3ds::read_back_frame): while the game is in first
// person, each finished left eye is converted by the GPU (a display
// transfer: untiled, RGBA8 to RGB5A1, the N64's RGBA16 bit layout) into one
// of two linear buffers in turn; the game thread reads the last complete one
// under g_readback_lock, which also keeps the GPU from refilling it
// meanwhile. Linear layout as the screen's: pixel (x, y) at x * 240 + 239 - y.
u16* g_readback[2] = { nullptr, nullptr };
int g_readback_ready = -1;              // the buffer holding the last finished frame
LightLock g_readback_lock;


int g_debug_gfx_delay_ms = 0;
volatile bool g_quitting = false;

// While the HOME Menu (or sleep) has the GPU, libctru has released the GPU
// rights: anything submitted then never completes and the gfx thread hangs
// in citro3d's queue wait (which also kept "Close" from ever finishing). The
// APT hook, on the main thread before the rights go, waits out the frame in
// flight under g_gpu_lock and raises the flags; send_dl then holds the game
// until the rights are back. The hook is registered after C3D_Init so it runs
// ahead of citro3d's own, which then drains the queue.
LightLock g_gpu_lock;
volatile bool g_suspended = false, g_sleeping = false;
aptHookCookie g_apt_cookie;

void apt_hook(APT_HookType type, void*) {
    switch (type) {
        case APTHOOK_ONSUSPEND:
        case APTHOOK_ONSLEEP:
            LightLock_Lock(&g_gpu_lock);
            if (type == APTHOOK_ONSUSPEND) g_suspended = true; else g_sleeping = true;
            LightLock_Unlock(&g_gpu_lock);
            break;
        case APTHOOK_ONRESTORE: g_suspended = false; break;
        case APTHOOK_ONWAKEUP:  g_sleeping = false; break;
        default: break;
    }
}

bool gpu_away() { return g_suspended || g_sleeping; }
volatile bool g_paused = false;
volatile bool g_capture_request = false;
volatile int g_seq_request = 0;         // rt64_3ds::request_frame_sequence
volatile bool g_pair_request = false;   // rt64_3ds::request_capture_pair
rt64_3ds::Progress g_progress{};
const rt64_3ds::DrawRecord* g_progress_draw = nullptr;

// ---- Frame captures on their way to the SD card (see maybe_capture and
// maybe_capture_framebuffer). The SD card takes seconds for a capture's
// 16 MB (about 1.5 MB/s on the console), so one writer thread, started with
// the first capture and kept for the session, writes them while the game
// plays on. It takes jobs from a two-entry queue (a "pair" capture queues
// two); a capture asked for while an earlier one is still being written is
// refused with a message for the touch screen, so nothing ever waits on the
// card but the writer. Files go through the FS service directly, in 256 KB
// requests, every result checked and logged (no stdio buffering, no newlib
// FILE or malloc locks on the writer's way). A thread per capture, writing
// 64 KB at a time through stdio, was what the console used to stop on
// partway through (always the sixth capture of a session, then a hard reset
// on quitting); Azahar wrote ten in a row either way.
struct CaptureJob {
    OSTask task{};
    uint8_t* rdram = nullptr;        // 16 MB copy, or null for an image-only capture
    uint8_t* frame = nullptr;
    size_t frame_bytes = 0;
    char frame_name[32] = {};        // gfx_frame.ppm, gfx_frame1.ppm, ...
    char task_name[32] = {};         // gfx_task.bin, gfx_task1.bin, ...: numbered like the frame image
};

namespace capture {
constexpr int kQueue = 2;
constexpr u32 kPiece = 256 * 1024;               // bytes per FS write request
LightLock g_lock = 1;                            // the queue and the message (never held over I/O); 1 = unlocked (LightLock_Init)
CaptureJob* g_queue[kQueue] = {};
int g_count = 0;                                 // jobs queued or being written (the writer pops when done)
Thread g_thread = nullptr;
volatile u32 g_done_bytes = 0, g_total_bytes = 0; // the job being written
volatile u64 g_progress_tick = 0;                // its last completed request
volatile bool g_abort = false;                   // quitting: stop between requests
volatile bool g_stall_noted = false;
volatile int g_state = 0;                        // 0 none, 1 writing, 2 written, 3 failed, 4 refused
char g_message[64] = "";
int g_message_seq = 0;
int g_throttle_ms = -1;                          // CAPTURE_THROTTLE.TXT: ms of sleep per request (emulating a slow card)
char g_writing[32] = "";                         // the file being written, for messages

void set_message(int state, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void set_message(int state, const char* fmt, ...) {
    LightLock_Lock(&g_lock);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_message, sizeof(g_message), fmt, ap);
    va_end(ap);
    g_message_seq++;
    if (state >= 0) g_state = state;
    LightLock_Unlock(&g_lock);
}

// One file, written from pieces through the FS service (sdmc:/... paths) or,
// for any other data folder, stdio. progress: account the bytes in
// g_done_bytes and log every 4 MB.
struct Piece { const void* data; u32 size; };
bool write_file(const char* name, const Piece* pieces, int n, bool progress) {
    char full[224];
    rt64_3ds::data_path(full, sizeof(full), name);
    const u64 t0 = svcGetSystemTick();
    if (strncmp(full, "sdmc:", 5) != 0) {
        FILE* f = fopen(full, "wb");
        bool ok = f != nullptr;
        for (int i = 0; i < n && ok; i++) ok = fwrite(pieces[i].data, 1, pieces[i].size, f) == pieces[i].size;
        if (f != nullptr && fclose(f) != 0) ok = false;
        if (!ok) fprintf(stderr, "rt64-3ds: capture: writing %s FAILED (errno %d)\n", full, errno);
        return ok;
    }
    Handle h = 0;
    Result rc = FSUSER_OpenFileDirectly(&h, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, full + 5),
                                        FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
    if (R_FAILED(rc)) {
        fprintf(stderr, "rt64-3ds: capture: opening %s FAILED (%08lx)\n", full, (unsigned long)rc);
        return false;
    }
    rc = FSFILE_SetSize(h, 0);
    u64 off = 0;
    u32 next_log = 4u << 20;
    for (int i = 0; i < n && R_SUCCEEDED(rc); i++) {
        for (u32 at = 0; at < pieces[i].size && R_SUCCEEDED(rc);) {
            if (g_abort) { rc = -1; fprintf(stderr, "rt64-3ds: capture: %s left at %llu bytes (quitting)\n", name, off); break; }
            const u32 len = std::min(kPiece, pieces[i].size - at);
            u32 written = 0;
            rc = FSFILE_Write(h, &written, off, (const u8*)pieces[i].data + at, len, 0);
            if (R_SUCCEEDED(rc) && written != len) rc = -2;
            if (R_FAILED(rc)) {
                fprintf(stderr, "rt64-3ds: capture: writing %s FAILED at %llu bytes (%08lx, %lu of %lu written)\n", name, off,
                        (unsigned long)rc, (unsigned long)written, (unsigned long)len);
                break;
            }
            at += len;
            off += len;
            g_progress_tick = svcGetSystemTick();
            if (g_throttle_ms > 0) svcSleepThread((s64)g_throttle_ms * 1000000);
            if (progress) {
                g_done_bytes += len;
                if (g_done_bytes >= next_log) {
                    next_log += 4u << 20;
                    const float s = (float)(svcGetSystemTick() - t0) / (float)SYSCLOCK_ARM11;
                    fprintf(stderr, "rt64-3ds: capture %s: %lu of %lu MB in %.1f s\n", name, (unsigned long)(g_done_bytes >> 20),
                            (unsigned long)(g_total_bytes >> 20), s);
                    set_message(-1, "Writing %s: %lu/%lu MB", name, (unsigned long)(g_done_bytes >> 20), (unsigned long)(g_total_bytes >> 20));
                }
            }
        }
    }
    const Result rc_close = FSFILE_Close(h);
    if (R_SUCCEEDED(rc) && R_FAILED(rc_close)) {
        fprintf(stderr, "rt64-3ds: capture: closing %s FAILED (%08lx)\n", name, (unsigned long)rc_close);
        rc = rc_close;
    }
    return R_SUCCEEDED(rc);
}

void write_job(CaptureJob* job) {
    const u64 t0 = svcGetSystemTick();
    bool ok = true;
    if (job->rdram != nullptr) {
        const Piece task[2] = { { &job->task, (u32)sizeof(OSTask) }, { job->rdram, 16u * 1024 * 1024 } };
        ok = write_file(job->task_name, task, 2, true);
    }
    if (job->frame != nullptr && !g_abort) {
        const Piece img[1] = { { job->frame, (u32)job->frame_bytes } };
        ok = write_file(job->frame_name, img, 1, false) && ok;
    }
    if (job->rdram == nullptr) return;       // image-only captures (sequences) say nothing
    const float s = (float)(svcGetSystemTick() - t0) / (float)SYSCLOCK_ARM11;
    if (ok) {
        fprintf(stderr, "rt64-3ds: capture written (%s, %s) in %.1f s (%.2f MB/s)\n", job->task_name, job->frame_name, s,
                s > 0.0f ? 16.0f / s : 0.0f);
        set_message(2, "Capture saved: %s (%.0f s)", job->task_name, s);
    }
    else {
        fprintf(stderr, "rt64-3ds: capture %s FAILED after %.1f s (%lu of %lu bytes)\n", job->task_name, s,
                (unsigned long)g_done_bytes, (unsigned long)g_total_bytes);
        set_message(3, "Capture FAILED: %s, see log", job->task_name);
    }
}

void free_job(CaptureJob* job) {
    free(job->rdram);
    free(job->frame);
    delete job;
}

void writer_main(void*) {
    for (;;) {
        CaptureJob* job = nullptr;
        LightLock_Lock(&g_lock);
        if (g_count > 0) job = g_queue[0];
        LightLock_Unlock(&g_lock);
        if (job == nullptr) {
            svcSleepThread(20 * 1000000ll);
            continue;
        }
        g_done_bytes = 0;
        g_total_bytes = job->rdram != nullptr ? 16u * 1024 * 1024 + (u32)sizeof(OSTask) : 0;
        g_progress_tick = svcGetSystemTick();
        g_stall_noted = false;
        snprintf(g_writing, sizeof(g_writing), "%s", job->task_name);
        write_job(job);
        free_job(job);
        LightLock_Lock(&g_lock);
        for (int i = 1; i < g_count; i++) g_queue[i - 1] = g_queue[i];
        g_queue[--g_count] = nullptr;
        LightLock_Unlock(&g_lock);
    }
}

// Jobs queued or being written.
int pending() {
    LightLock_Lock(&g_lock);
    const int n = g_count;
    LightLock_Unlock(&g_lock);
    return n;
}

// Hands a full capture to the writer (starting it the first time); false
// (and the job is the caller's still) when the queue is full or there is no
// writer.
bool queue(CaptureJob* job) {
    if (g_thread == nullptr) {
        if (g_throttle_ms < 0) {
            g_throttle_ms = 0;
            if (FILE* f = rt64_3ds::data_fopen("CAPTURE_THROTTLE.TXT", "r")) {
                if (fscanf(f, "%d", &g_throttle_ms) != 1) g_throttle_ms = 0;
                fclose(f);
                fprintf(stderr, "rt64-3ds: capture writer throttled: %d ms per %lu KB\n", g_throttle_ms, (unsigned long)(kPiece / 1024));
            }
        }
        // Core 0 just above the game's threads, as the log writer: it
        // mostly waits on the card.
        g_thread = threadCreate(writer_main, nullptr, 32 * 1024, 0x2F, 0, false);
        if (g_thread == nullptr) {
            fprintf(stderr, "rt64-3ds: capture writer thread FAILED to start\n");
            return false;
        }
    }
    LightLock_Lock(&g_lock);
    const bool room = g_count < kQueue;
    if (room) {
        if (g_count == 0) {
            // (The writer restarts these when it takes the job: until then
            // the stall check must not see the last job's time.)
            g_progress_tick = svcGetSystemTick();
            g_stall_noted = false;
        }
        g_queue[g_count++] = job;
        g_state = 1;
    }
    LightLock_Unlock(&g_lock);
    return room;
}

// The touch screen's line, with a note once when the writer has made no
// progress for 10 s (a stuck card or FS request: the game plays on, further
// captures are refused, the file stays incomplete).
const char* message(int* seq) {
    if (pending() > 0 && !g_stall_noted && svcGetSystemTick() - g_progress_tick > 10ull * SYSCLOCK_ARM11) {
        g_stall_noted = true;
        fprintf(stderr, "rt64-3ds: capture writer STALLED: no progress for 10 s on %s at %lu of %lu bytes\n", g_writing,
                (unsigned long)g_done_bytes, (unsigned long)g_total_bytes);
        set_message(-1, "Capture stalled: %s %lu/%lu MB", g_writing, (unsigned long)(g_done_bytes >> 20), (unsigned long)(g_total_bytes >> 20));
    }
    LightLock_Lock(&g_lock);
    *seq = g_message_seq;
    LightLock_Unlock(&g_lock);
    return g_message;
}

}   // namespace capture

// The process's kernel objects in use against its limits (an application
// may own 32 threads, events and mutexes, 8 semaphores and timers).
void log_kernel_objects(const char* when) {
    Handle rl = 0;
    if (R_FAILED(svcGetResourceLimit(&rl, CUR_PROCESS_HANDLE))) return;
    ResourceLimitType names[] = { RESLIMIT_THREAD, RESLIMIT_EVENT, RESLIMIT_MUTEX, RESLIMIT_SEMAPHORE, RESLIMIT_TIMER, RESLIMIT_SHAREDMEMORY, RESLIMIT_ADDRESSARBITER };
    constexpr int n = sizeof(names) / sizeof(names[0]);
    s64 cur[n] = {}, lim[n] = {};
    const bool ok = R_SUCCEEDED(svcGetResourceLimitCurrentValues(cur, rl, names, n)) && R_SUCCEEDED(svcGetResourceLimitLimitValues(lim, rl, names, n));
    svcCloseHandle(rl);
    if (!ok) return;
    fprintf(stderr, "rt64-3ds: kernel objects %s: threads %lld/%lld events %lld/%lld mutexes %lld/%lld semaphores %lld/%lld timers %lld/%lld shared mem %lld/%lld arbiters %lld/%lld\n",
            when, cur[0], lim[0], cur[1], lim[1], cur[2], lim[2], cur[3], lim[3], cur[4], lim[4], cur[5], lim[5], cur[6], lim[6]);
}

// A screenshot on its way to the SD card (see maybe_screenshot): up to three
// BMP files built in memory, written by a background thread.
struct ShotJob {
    uint8_t* file[3] = {};
    size_t bytes[3] = {};
    char path[3][224] = {};
};
volatile bool g_shot_request = false;
volatile int g_shot_state = 0;       // 0 idle, 1 writing, 2 written, 3 failed
char g_shot_name[48] = "";

// A 24-bit BMP of w x h pixels, pixel(x, y) from the top left giving RGB.
template <class F>
uint8_t* make_bmp(int w, int h, size_t* bytes, F pixel) {
    const uint32_t row = (uint32_t)w * 3, pad = (4 - row % 4) % 4, size = 54 + (row + pad) * (uint32_t)h;
    uint8_t* b = (uint8_t*)calloc(1, size);
    if (b == nullptr) return nullptr;
    auto put32 = [&](int at, uint32_t v) { b[at] = v & 0xFF; b[at + 1] = (v >> 8) & 0xFF; b[at + 2] = (v >> 16) & 0xFF; b[at + 3] = v >> 24; };
    b[0] = 'B'; b[1] = 'M'; put32(2, size); put32(10, 54);
    put32(14, 40); put32(18, (uint32_t)w); put32(22, (uint32_t)h);
    b[26] = 1; b[28] = 24; put32(34, size - 54);
    uint8_t* o = b + 54;
    for (int i = 0; i < h; i++) {       // bottom row first
        const int y = h - 1 - i;
        for (int x = 0; x < w; x++) {
            uint8_t rgb[3];
            pixel(x, y, rgb);
            *o++ = rgb[2]; *o++ = rgb[1]; *o++ = rgb[0];
        }
        o += pad;
    }
    *bytes = size;
    return b;
}

void write_shot(void* arg) {
    ShotJob* job = static_cast<ShotJob*>(arg);
    bool ok = true;
    for (int i = 0; i < 3; i++) {
        if (job->file[i] == nullptr) continue;
        FILE* f = fopen(job->path[i], "wb");
        ok = ok && f != nullptr && fwrite(job->file[i], 1, job->bytes[i], f) == job->bytes[i];
        if (f != nullptr) fclose(f);
        free(job->file[i]);
    }
    fprintf(stderr, "rt64-3ds: screenshot %s %s\n", g_shot_name, ok ? "written" : "FAILED");
    g_shot_state = ok ? 2 : 3;
    delete job;
}

class C3dRenderContext final : public ultramodern::renderer::RendererContext {
public:
    C3dRenderContext(uint8_t* rdram) : rdram_(rdram), interp_(rdram), textures_(rdram) {
        setup_result = ultramodern::renderer::SetupResult::Success;
        init();
    }
    ~C3dRenderContext() override {}

    bool valid() override { return ok_; }
    bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override { return true; }
    void enable_instant_present() override {}
    void send_dummy_workload(uint32_t fb_address) { (void)fb_address; }
    void update_screen() override {}
    // Runs on the gfx thread when the runtime quits: let the GPU finish what
    // it was given and give citro3d's hooks and queues back before the
    // process closes (C3D_Fini waits for the render queue).
    void shutdown() override {
        if (!ok_) return;
        stop_reader();
        stop_builder();
        stop_presenter();
        ok_ = false;
        aptUnhook(&g_apt_cookie);
        C3D_Fini();
        fprintf(stderr, "rt64-3ds: renderer shut down\n");
    }
    uint32_t get_display_framerate() const override { return 60; }
    float get_resolution_scale() const override { return 1.0f; }

    void send_dl(const OSTask* task) override {
        if (!ok_ || g_quitting) return;
        // Hold the game while the GPU is away (it would only run ahead unseen)
        // or a prompt has it paused.
        while ((gpu_away() || g_paused) && !g_quitting) svcSleepThread(10 * 1000000ll);
        if (g_quitting) return;
        u64 t0 = svcGetSystemTick();
        vsync_at_dl_ = acc_vsync_;
        list_seq_++;
        // How long the list waited for this thread (it was queued while the
        // one before was still drawn: with RenderDesc::early_drawn the game
        // does not wait for that), and when the game submitted it.
        const u64 wait = (u64)ultramodern::renderer::list_wait_us() * SYSCLOCK_ARM11 / 1000000ull;
        const u64 submitted = t0 - std::min(wait, t0 - last_dl_tick_);
        // The game's frame time at the rate it asked for last (30 fps if none).
        const uint32_t last_rate = interp_.stats().refresh_rate;
        const float period_ms = (last_rate >= 15 && last_rate < 60) ? 1000.0f / (float)last_rate : 1000.0f / 30.0f;
        // Frame pacing: a gap well past the game's frame time (two VIs at
        // least) is a frame the game delivered late (its pacing loses that
        // time, e.g. against the intro music).
        if (last_dl_tick_ != 0) {
            float gap = (float)(submitted - last_dl_tick_) * 1000.0f / (float)SYSCLOCK_ARM11;
            if (gap > std::max(50.0f, period_ms + 8.0f)) {
                late_++;
                if (gap > max_gap_) max_gap_ = gap;
                if (late_logged_ < 200) {
                    late_logged_++;
                    fprintf(stderr, "rt64-3ds: late frame %u: %.1f ms since the previous list (that one: read %.1f, tris %.1f, interp %.1f, replay %.1f ms, triangles drained in the read %.1f; this one waited %.1f)\n",
                            frames_, gap, last_read_ms_, last_tris_ms_, last_interp_ms_, last_replay_ms_, last_drain_ms_, (float)wait * 1000.0f / (float)SYSCLOCK_ARM11);
                }
            }
        }
        last_dl_tick_ = submitted;
        // Frame interpolation (RenderDesc::frame_interpolation, Settings::
        // frame_cap 2): the interpreter records each list's transforms.
        const bool interp_mode = rt64_3ds::render_desc().frame_interpolation && rt64_3ds::settings().frame_cap == 2;
        if (interp_mode != interp_mode_) {
            interp_.set_interpolation(interp_mode);
            interp_mode_ = interp_mode;
            prev_rate_ = 0;
            prev_list_ok_ = false;
        }
        // The presents are behind the game, a little more every list (the
        // game's 60 Hz against the screen's 59.83): this list gets no
        // in-between frame, which brings them a refresh nearer. Without
        // early_drawn the game waits for them, and a list that arrived the
        // moment the last one was done was held back. With it the game goes
        // on and a list may wait for the one before a while; one that waited
        // more than 40% of the game's frame time would soon hold the game on
        // "parsed" (its next frame is built once this one is read).
        const bool queued = early_drawn_
                                ? (float)wait * 1000.0f / (float)SYSCLOCK_ARM11 > 0.4f * period_ms
                                : last_pair_end_ != 0 && t0 - last_pair_end_ < SYSCLOCK_ARM11 / 1000;
        // While a probe runs (make_inbetween, over budget), a list that only
        // waited for the one before still gets its in-between: the probe's
        // first pairs overrun, before the builder's work hides in the waits
        // for the screen, so the lists after them wait; skipping those kept
        // probes from reaching the steady running they are there to measure
        // (Banjo's file select: 25-27 ms a pair running, 31-38 alone). A
        // resync with the screen still counts.
        const bool behind = interp_mode_ && ((queued && probe_left_ == 0) || resync_);
        behind_resync_ = resync_;
        resync_ = false;
        // The read, up to "parsed" (on the reader thread when there is one).
        depth_read_early_ = false;
        const ReadOutcome read = read_list(task);
        if (read == ReadOutcome::PassedOver) {
            g_progress.phase = 0;
            const u64 tp = svcGetSystemTick();
            acc_pass_ += tp - t0;
            dl_count_++;
            passed_count_++;
            report_window(t0, tp);
            return;
        }
        const bool present = read == ReadOutcome::Drawn;
        // The triangles the reader left for this thread (deferred triangles,
        // with a reader): their vertices, after "parsed".
        const u64 t_read = svcGetSystemTick();
        if (interp_.finish_triangles(frame_)) {
            const u64 tt = svcGetSystemTick() - t_read;
            acc_tris_ += tt;
            last_tris_ms_ = (float)tt * 1000.0f / (float)SYSCLOCK_ARM11;
        }
        else last_tris_ms_ = 0.0f;
        // Frame interpolation: the in-betweens, from the list's records alone
        // (no RDRAM). They draw the game frame's draws with its textures.
        const u64 t_run = svcGetSystemTick(), tb0 = t_run;
        engaged_ = false;
        const bool inbetween = interp_mode_ && present && make_inbetween(task, behind);
        const u64 tb1 = svcGetSystemTick();
        if (inbetween) {
            acc_inb_interp_ += tb1 - tb0;
            inter_bound_ = bound_;
        }
        await_presenter();
        early_depth();
        if (seq_rtt_count_ > 0 && frames_ > 10) {
            for (const rt64_3ds::DrawRecord& d : frame_.draws) {
                if (d.rtt >= 0) { seq_first_ = (int)frames_ + 1; seq_count_ = seq_rtt_count_; seq_rtt_count_ = 0; break; }
            }
        }
        if (rtt_capture_armed_ && frames_ > 10 && capture_copy_ != nullptr) {
            for (const rt64_3ds::DrawRecord& d : frame_.draws) {
                if (d.rtt >= 0) {
                    rtt_capture_armed_ = false;
                    capture_task_ = rtt_capture_task_;
                    fb_capture_pending_ = true;
                    fprintf(stderr, "rt64-3ds: captured gfx task of frame %u (it draws off screen)\n", frames_);
                    break;
                }
            }
        }
        if (frame_.snapshot_request) {
            fprintf(stderr, "rt64-3ds: frame %u stores the screen (cimg %06x)\n", frames_, frame_.color_image);
            if (store_delay_ >= 0 && capture_want_ < 0) { capture_want_ = (int)frames_ + store_delay_; store_delay_ = -1; }
            // STORE_DEBUG.TXT: every draw of the frame 3 frames after each store.
            static int store_debug = -1;
            if (store_debug < 0) {
                FILE* f = rt64_3ds::data_fopen("STORE_DEBUG.TXT", "r");
                store_debug = f ? 1 : 0;
                if (f) fclose(f);
            }
            if (store_debug == 1) { gpu_debug_ = true; debug_from_ = frames_ + 3; debug_count_ = 1; debug_sync_ = false; debug_step_ = 1; }
        }
        u64 t1 = svcGetSystemTick();
        dl_count_++;
        if (cam_log_now_) {
            cam_log_now_ = false;
            const float* c = frame_.cam;
            u64 now = svcGetSystemTick();
            static u64 last = 0;
            // DK64 (debug only): the camera eye/target from character_change_array (0x807FC924) + 0x210 / 0x228.
            auto rf = [&](uint32_t va) { float f; uint32_t w = *(const uint32_t*)(rdram_ + ((va - 0x80000000u) & 0xFFFFFC)); memcpy(&f, &w, 4); return f; };
            uint32_t cc = *(const uint32_t*)(rdram_ + (0x807FC924u - 0x80000000u));
            float ex = 0, ey = 0, ez = 0, ax = 0, ay = 0, az = 0;
            if (cc >= 0x80000000u && cc < 0x80800000u) { ex = rf(cc + 0x210); ey = rf(cc + 0x214); ez = rf(cc + 0x218); ax = rf(cc + 0x228); ay = rf(cc + 0x22C); az = rf(cc + 0x230); }
            (void)c;
            fprintf(stderr, "cam %u: dt %.1f ms eye %.2f %.2f %.2f at %.2f %.2f %.2f\n", frames_,
                    last ? (double)(now - last) * 1000.0 / SYSCLOCK_ARM11 : 0.0, ex, ey, ez, ax, ay, az);
            last = now;
        }
        u64 replay_real = 0, replay_inb = 0;
        int inbs = 0;                   // in-betweens drawn
        if (present) {
            LightLock_Lock(&g_gpu_lock);
            bool handed = false;
            if (!gpu_away() && !g_quitting) {
                if (inbetween) {
                    // The in-between frames, then the game's own, a refresh
                    // apart: each waits until the frame before it is on the
                    // screen (it has two buffers). The game's frame is handed
                    // to the presenter thread for that wait, so the game
                    // hears its list is done without waiting for a refresh.
                    // An in-between after the first is built from the same
                    // matches at its own weight while the one before is on
                    // the GPU, into the other in-between vertex buffer (the
                    // one before that is done: its successor's frame began
                    // after it).
                    // With the builder (core 1) building the first in-between,
                    // the game's frame is recorded first, meanwhile, then the
                    // in-betweens, and both go to the presenter: the GPU runs
                    // them in the order they are queued (the in-betweens',
                    // then the game frame's, which the presenter queues), not
                    // the order they were recorded in, and each frame sets
                    // all of its state at its start.
                    const bool game_first = build_busy_;
                    u64 waited_first = 0, built = 0;
                    pair_ = true;
                    if (game_first) {
                        const u64 ta0 = svcGetSystemTick();
                        record_only_ = true;
                        replay_and_present(Present::Paced, true);
                        record_only_ = false;
                        replay_real = svcGetSystemTick() - ta0;
                        // What of the build the recording did not cover.
                        const u64 tj0 = svcGetSystemTick();
                        const bool ok = join_build();
                        built = svcGetSystemTick() - tj0;
                        acc_build_wait_ += built;
                        builds_++;
                        if (!ok) {
                            if (build_skip_ == 1) inb_cut_++;
                            else if (build_skip_ == 2) inb_nothing_++;
                            else inb_diverged_++;
                            inb_count_ = 0;
                        }
                    }
                    const u64 tr0 = svcGetSystemTick(), w0 = acc_vsync_;
                    u64 built_here = 0;
                    for (int k = 1; k <= inb_count_; k++) {
                        if (k > 1) {
                            const u64 tk0 = svcGetSystemTick();
                            interp_.reweight_previous((float)k / (float)(inb_count_ + 1));
                            inter_.clear();
                            inter_.verts.set_storage((rt64_3ds::Vtx3ds*)vbo_[2 + ((k - 1) & 1)], kVboVerts);
                            const bool ok = interp_.build_inbetween(frame_, inter_);
                            built_here += svcGetSystemTick() - tk0;
                            if (!ok) { inb_diverged_++; break; }
                        }
                        // The last in-between's transfer is the presenter's
                        // (double eyes): its wait for the screen is not this
                        // thread's.
                        const bool hold = k == inb_count_ && double_eyes_ && presenter_ != nullptr;
                        const u64 wk0 = acc_vsync_;
                        swap_inbetween();
                        replay_and_present(Present::Inbetween, false, hold);
                        swap_inbetween();
                        if (k == 1) waited_first = acc_vsync_ - wk0;
                        inb_drawn_++;
                        inbs++;
                    }
                    const u64 waited = acc_vsync_ - w0;
                    replay_inb = svcGetSystemTick() - tr0 - waited - built_here;
                    built += built_here;
                    acc_inb_replay_ += replay_inb;
                    acc_inb_interp_ += built;
                    inb_built_ = built;
                    // The first in-between waited this long for the frame
                    // before it: the presents run late against the game, which
                    // is a little faster than the screen's refreshes (60 Hz
                    // against 59.83). The next list goes without in-betweens,
                    // which brings them a refresh nearer. (The ones after wait
                    // for the in-between before them by design.)
                    if (waited_first > (u64)SYSCLOCK_ARM11 * 8 / 1000) resync_ = true;
                    if (game_first) handed = hand_off_recorded(inbs > 0);
                    else {
                        const u64 ta0 = svcGetSystemTick(), wa0 = acc_vsync_;
                        handed = replay_and_present(Present::Paced, presenter_ != nullptr);
                        replay_real = svcGetSystemTick() - ta0 - (acc_vsync_ - wa0);
                    }
                    pair_ = false;
                }
                else {
                    const u64 ta0 = svcGetSystemTick(), wa0 = acc_vsync_;
                    replay_and_present(engaged_ ? Present::Paced : Present::Normal);
                    replay_real = svcGetSystemTick() - ta0 - (acc_vsync_ - wa0);
                }
            }
            if (!handed) LightLock_Unlock(&g_gpu_lock);
        }
        // A build begun for a list not drawn after all (the GPU away, quitting).
        if (build_busy_) join_build();
        if (g_debug_gfx_delay_ms > 0) svcSleepThread((s64)g_debug_gfx_delay_ms * 1000000);
        g_progress.phase = 0;
        u64 t2 = svcGetSystemTick();
        acc_list_busy_ += t2 - t0;
        if (t2 - t0 > max_list_busy_) max_list_busy_ = t2 - t0;
        acc_list_read_ += t_read - t0;
        if (t_read - t0 > max_list_read_) max_list_read_ = t_read - t0;
        last_read_ms_ = (float)(t_read - t0) * 1000.0f / (float)SYSCLOCK_ARM11;
        acc_list_wait_ += wait;
        if (wait > max_list_wait_) max_list_wait_ = wait;
        if (engaged_) last_pair_end_ = t2;
        else last_pair_end_ = 0;
        if (engaged_) {
            // What the game's frame and an in-between cost the renderer (not
            // counting the waits for the screen), to drop in-betweens a scene
            // that heavy can't afford.
            const float ms = 1000.0f / (float)SYSCLOCK_ARM11;
            const float real = (float)((t_run - t0) + replay_real) * ms;
            real_ema_ = real_ema_ <= 0.0f ? real : real_ema_ + (real - real_ema_) * 0.25f;
            if (inbs > 0) {
                // An in-between's cost as a share of the game frame's (it
                // follows the scene as that does, measured or not); a frame
                // that cost far more (a capture, a hitch) does not count.
                const float inbs_ms = (float)((tb1 - tb0) + inb_built_ + replay_inb) * ms;
                const float share = inbs_ms / (float)inbs / std::max(real, 0.1f);
                if (share < 3.0f) inb_share_ = inb_share_ <= 0.0f ? share : inb_share_ + (share - inb_share_) * 0.25f;
                pair_ms_ = real + inbs_ms;
            }
            inb_ema_ = real_ema_ * (inb_share_ > 0.0f ? inb_share_ : 1.0f);
            pair_ema_ = real_ema_ + (float)std::max(inbs, 1) * inb_ema_;
        }
        acc_gfx_ += t1 - t0;
        acc_replay_ += t2 - t1;
        last_interp_ms_ = (float)(t1 - t0) * 1000.0f / (float)SYSCLOCK_ARM11;
        last_replay_ms_ = (float)(t2 - t1) * 1000.0f / (float)SYSCLOCK_ARM11;
        report_window(t0, t2);
    }

    // The per-second figures (FrameStats) and, every fifth second, the log.
    void report_window(u64 t0, u64 t2) {
        if (window_start_ == 0) window_start_ = t0;
        else if (t2 - window_start_ >= SYSCLOCK_ARM11) {
            g_stats.dl_per_sec = (int)dl_count_;
            g_stats.game_fps = (int)interp_.stats().refresh_rate;
            g_stats.late_frames = (int)late_;
            g_stats.max_gap_ms = max_gap_;
            late_ = 0;
            max_gap_ = 0;
            // Times per list drawn (the frame cap's passed-over lists apart).
            const uint32_t drawn = dl_count_ - passed_count_;
            const float per = 1000.0f / (float)SYSCLOCK_ARM11 / (float)(drawn ? drawn : 1);
            g_stats.gfx_ms = (float)acc_gfx_ * per;
            g_stats.replay_ms = (float)acc_replay_ * per;
            wait_ms_ = (float)acc_wait_ * per;
            end_ms_ = (float)acc_end_ * per;
            depth_ms_ = (float)acc_depth_ * per;
            g_stats.dl_skipped_per_sec = (int)passed_count_;
            // Frame interpolation: frames on the screen, in-betweens among
            // them, and lists the game asked 30 for that got none.
            const uint32_t inb_missed = inb_late_ + inb_budget_ + inb_unsafe_ + inb_unrelated_ + inb_cut_ + inb_nothing_ + inb_diverged_;
            g_stats.presented_per_sec = (int)presented_;
            g_stats.inbetween_per_sec = (int)inb_drawn_;
            g_stats.inbetween_dropped_per_sec = (int)inb_missed;
            {
                // Why lists went without one, the non-zero reasons.
                char* o = g_stats.inbetween_reasons;
                const size_t cap = sizeof(g_stats.inbetween_reasons);
                size_t at = 0;
                o[0] = 0;
                auto add = [&](const char* name, uint32_t n) { if (n != 0 && at + 24 < cap) at += snprintf(o + at, cap - at, "%s%s %u", at ? " " : "", name, n); };
                add("late", inb_late_); add("budget", inb_budget_); add("snapshot", inb_snapshot_); add("rtt", inb_rtt_);
                add("offscreen", inb_offscreen_); add("blanked", inb_blank_); add("depth", inb_depth_); add("after60", inb_after60_);
                add("after-unsafe", inb_after_unsafe_); add("cut", inb_cut_); add("untagged", inb_nothing_); add("diverged", inb_diverged_);
                inb_snapshot_ = inb_rtt_ = inb_offscreen_ = inb_blank_ = inb_depth_ = inb_after60_ = inb_after_unsafe_ = 0;
            }
            {
                const float ms = 1000.0f / (float)SYSCLOCK_ARM11;
                const float per_inb = inb_drawn_ ? ms / (float)inb_drawn_ : 0.0f;
                const uint32_t lists = dl_count_ - passed_count_;
                snprintf(interp_line_, sizeof(interp_line_),
                         "rt64-3ds:   frame interpolation %s: last second %u frames shown, %u in-between (interp+match %.1f ms, replay %.1f ms each), "
                         "%u lists without one (late %u [%u to resync with the screen, the rest %s], over budget %u [%u runs tried anyway, next after %u], unsafe %u, "
                         "after a 60 or unsafe list %u, camera cut %u, nothing tagged %u, diverged %u)\n"
                         "rt64-3ds:   frame interpolation timing: waits for the screen %.1f ms per list, gfx thread %.1f ms per list (max %.1f; reading it %.1f, max %.1f), lists waited %.1f ms for the one before (max %.1f), "
                         "pair %.1f ms (expected %.1f); first in-betweens matched and built on core 1 %u (%.1f ms each, %.1f of it waited for here); "
                         "halves built on core 3 %u (%.1f ms each, %.1f of it waited for)\n"
                         "rt64-3ds:   frame interpolation last match: transforms %u/%u/%u (vertices %u), projections %u/%u/%u, tiles %u/%u",
                         interp_mode_ ? "on" : "off", presented_, inb_drawn_, (float)(acc_inb_interp_) * per_inb, (float)acc_inb_replay_ * per_inb,
                         inb_missed, inb_late_, inb_late_resync_,
                         early_drawn_ ? "waited too long for the one before" : "arrived while the last one was still drawn",
                         inb_budget_, probes_, probe_every_, inb_unsafe_, inb_unrelated_, inb_cut_, inb_nothing_, inb_diverged_,
                         lists ? (float)(acc_vsync_ + (u64)presenter_wait_us_ * SYSCLOCK_ARM11 / 1000000ull) * ms / (float)lists : 0.0f,
                         lists ? (float)acc_list_busy_ * ms / (float)lists : 0.0f, (float)max_list_busy_ * ms,
                         lists ? (float)acc_list_read_ * ms / (float)lists : 0.0f, (float)max_list_read_ * ms,
                         lists ? (float)acc_list_wait_ * ms / (float)lists : 0.0f, (float)max_list_wait_ * ms, pair_ms_, pair_ema_,
                         builds_, builds_ ? (float)build_ticks_ * ms / (float)builds_ : 0.0f, builds_ ? (float)acc_build_wait_ * ms / (float)builds_ : 0.0f,
                         helper_.jobs, helper_.jobs ? (float)helper_.run_ticks * ms / (float)helper_.jobs : 0.0f,
                         helper_.jobs ? (float)helper_.wait_ticks * ms / (float)helper_.jobs : 0.0f,
                         last_match_.xf, last_match_.xf_matched, last_match_.xf_interp, last_match_.vert_entries,
                         last_match_.pj, last_match_.pj_matched, last_match_.pj_interp, last_match_.tiles, last_match_.tiles_interp);
                presented_ = inb_drawn_ = inb_late_ = inb_budget_ = inb_unsafe_ = inb_unrelated_ = inb_cut_ = inb_nothing_ = inb_diverged_ = 0;
                inb_late_resync_ = probes_ = 0;
                acc_list_busy_ = max_list_busy_ = acc_list_read_ = max_list_read_ = acc_list_wait_ = max_list_wait_ = 0;
                acc_build_wait_ = 0;
                build_ticks_ = 0;
                builds_ = 0;
                helper_.jobs = 0;
                helper_.run_ticks = 0;
                helper_.wait_ticks = 0;
                acc_inb_interp_ = acc_inb_replay_ = acc_match_ = acc_vsync_ = 0;
                presenter_wait_us_ = 0;
                vsync_at_dl_ = 0;
            }
            last_passed_ = passed_count_;
            last_refused_ = pass_refused_;
            pass_ms_ = passed_count_ ? (float)acc_pass_ * 1000.0f / (float)SYSCLOCK_ARM11 / (float)passed_count_ : 0.0f;
            dl_count_ = 0;
            passed_count_ = pass_refused_ = 0;
            acc_gfx_ = acc_replay_ = acc_wait_ = acc_end_ = acc_depth_ = acc_pass_ = 0;
            window_start_ = t2;
            const rt64_3ds::InterpreterStats& is = interp_.stats();
            frames_in_window_ = (float)(frames_ - frames_at_window_ > 0 ? frames_ - frames_at_window_ : 1);
            frames_at_window_ = frames_;
            if (++report_ % 5 == 1) {
                float fpw = (float)(frames_ - frames_at_profile_ > 0 ? frames_ - frames_at_profile_ : 1);
                frames_at_profile_ = frames_;
                if (prof_on_) {
                    // The list's read by kind of command (what the game waits
                    // for, to "parsed"), per list drawn.
                    const uint64_t* rp = interp_.stats().prof;
                    const float pl = 1000.0f / (float)SYSCLOCK_ARM11 / fpw;
                    fprintf(stderr, "rt64-3ds:   read profile per list: vertices %.1f triangles %.1f matrices %.1f texture loads %.1f rects %.1f "
                                    "list flow %.1f extended %.1f other %.1f, texture decoding %.1f ms; deferred triangles %.1f ms after it\n",
                            (float)rp[0] * pl, (float)rp[1] * pl, (float)rp[2] * pl, (float)rp[3] * pl, (float)rp[4] * pl, (float)rp[5] * pl,
                            (float)rp[6] * pl, (float)rp[7] * pl, (float)acc_prefetch_ * pl, (float)acc_tris_ * pl);
                    interp_.clear_profile();
                    acc_prefetch_ = 0;
                    acc_tris_ = 0;
                }
                if (prof_on_) fprintf(stderr, "rt64-3ds:   replay profile per frame: textures %.1f tev %.1f state %.1f flush %.1f draw %.1f ms\n",
                        (float)prof_[0] * 1000.0f / SYSCLOCK_ARM11 / fpw, (float)prof_[1] * 1000.0f / SYSCLOCK_ARM11 / fpw,
                        (float)prof_[2] * 1000.0f / SYSCLOCK_ARM11 / fpw, (float)prof_[4] * 1000.0f / SYSCLOCK_ARM11 / fpw,
                        (float)prof_[3] * 1000.0f / SYSCLOCK_ARM11 / fpw);
                fprintf(stderr, "rt64-3ds: frame %u: %u draws %u verts, interp %.1f ms replay %.1f ms (gpu wait %.1f, end %.1f, depth %.1f), tex live %u (%u KB, %u in VRAM, %u mipmapped) %u uploads/frame, linear free %u KB, unknown ops %u ex %u, tev fallbacks %d, cimg %06x w%u, snapshots %u, lod branches %u/%u, culldl %u\n",
                        frames_, (unsigned)last_draws_, (unsigned)last_verts_, g_stats.gfx_ms, g_stats.replay_ms, wait_ms_, end_ms_, depth_ms_, textures_.live(), textures_.bytes() / 1024, textures_.vram_bytes() / 1024, textures_.mipmapped(), last_uploads_,
                        (unsigned)(linearSpaceFree() / 1024), is.unknown, is.ex_unknown, g_stats.combiner_fallbacks, frame_.color_image, frame_.color_width, snapshots_,
                        is.branch_z_taken, is.branch_z_taken + is.branch_z_not, is.cull_dl);
                {
                    // GPU time per frame drawn and per eye (C3D_GetDrawingTime),
                    // game frames and in-betweens apart; on the console these
                    // are the PICA's, in Azahar the host's.
                    const uint32_t fr = gpu_frames_[0] + gpu_frames_[1], ey = gpu_eyes_[0] + gpu_eyes_[1];
                    const float all = gpu_ms_[0] + gpu_ms_[1];
                    char split[96] = "";
                    if (gpu_frames_[1] != 0)
                        snprintf(split, sizeof(split), "; game frames %.1f ms, in-betweens %.1f ms", gpu_frames_[0] ? gpu_ms_[0] / (float)gpu_frames_[0] : 0.0f,
                                 gpu_ms_[1] / (float)gpu_frames_[1]);
                    const rt64_3ds::Settings& gs = rt64_3ds::settings();
                    char fills[32] = "off";
                    if (fill_clear_) snprintf(fills, sizeof(fills), "%u of %u frames", fill_cleared_, fr);
                    fprintf(stderr, "rt64-3ds:   gpu %.2f ms a frame drawn, %.2f an eye (%u frames, %.1f eyes each)%s; eyes %s/%s, I/IA textures %s, ghost pass %s, fill clears %s\n",
                            fr ? all / (float)fr : 0.0f, ey ? all / (float)ey : 0.0f, fr, fr ? (float)ey / (float)fr : 0.0f, split,
                            color16_ ? "RGB565" : "RGBA8", depth16_ ? "D16" : "D24S8", textures_.compact_intensity() ? "LA8" : "RGBA8",
                            (gs.ghost_contrast < 100 || gs.ghost_black_floor > 0) ? "on" : "off", fills);
                    fill_cleared_ = 0;
                    gpu_ms_[0] = gpu_ms_[1] = 0.0f;
                    gpu_frames_[0] = gpu_frames_[1] = gpu_eyes_[0] = gpu_eyes_[1] = 0;
                }
                if (interp_mode_) {
                    // An interpolated pair's GPU waits on the gfx thread
                    // (the game's path): the in-between's C3D_FrameBegin
                    // waits for the game frame before it, the game frame's
                    // for the in-between (RenderDesc::double_eyes takes that
                    // one off the gfx thread: the presenter waits instead).
                    const float ms = 1000.0f / (float)SYSCLOCK_ARM11;
                    const float pp = pair_frames_ ? ms / (float)pair_frames_ : 0.0f;
                    const float pd = deferred_frames_ ? ms / (float)deferred_frames_ : 0.0f;
                    char dbl[320] = "off";
                    if (double_eyes_) {
                        snprintf(dbl, sizeof(dbl), "on: %u game frames recorded while their in-between drew; the presenter waited %.1f ms each for the "
                                 "in-between's GPU work, %.1f for the frame's draws; in-between vertices waited %.1f ms a pair; command buffer %u of %u KB; "
                                 "in-betweens transferred by the presenter %u, here %u",
                                 deferred_frames_, (float)acc_presenter_gpu_wait_[0] * pd, (float)acc_presenter_gpu_wait_[1] * pd, (float)acc_vbo_wait_ * pp,
                                 (unsigned)(dbuf_used_max_ * 4 / 1024), (unsigned)(kDeferBytes / 1024), held_presented_, held_flushed_);
                    }
                    fprintf(stderr, "rt64-3ds:   pairs' gpu waits on the gfx thread: in-between %.1f ms, game frame %.1f ms (%u pairs); double eyes %s\n",
                            (float)acc_pair_wait_[1] * pp, (float)acc_pair_wait_[0] * pp, pair_frames_, dbl);
                }
                acc_pair_wait_[0] = acc_pair_wait_[1] = acc_presenter_gpu_wait_[0] = acc_presenter_gpu_wait_[1] = acc_vbo_wait_ = 0;
                pair_frames_ = deferred_frames_ = 0;
                held_presented_ = held_flushed_ = 0;
                dbuf_used_max_ = 0;
                if (last_passed_ != 0 || last_refused_ != 0 || rt64_3ds::settings().frame_cap == 1) {
                    fprintf(stderr, "rt64-3ds:   frame cap %s: last second %u of %u lists drawn, %u passed over (%.1f ms each), %u could not be (not a whole frame on the screen); game asks %u\n",
                            rt64_3ds::settings().frame_cap == 1 ? "30 fps" : "off", (unsigned)(g_stats.dl_per_sec - (int)last_passed_), (unsigned)g_stats.dl_per_sec,
                            last_passed_, pass_ms_, last_refused_, is.refresh_rate);
                }
                if (interp_mode_) fprintf(stderr, "%s\n", interp_line_);
                {
                    const float fw = (float)(frames_ - frames_at_stereo_ > 0 ? frames_ - frames_at_stereo_ : 1);
                    frames_at_stereo_ = frames_;
                    const rt64_3ds::Settings& st = rt64_3ds::settings();
                    fprintf(stderr, "rt64-3ds:   stereo draws: world %u infinity %u overlay %u hud %u bubble %u rect %u none %u reticle %u%s\n",
                            class_count_[1], class_count_[2], class_count_[3], class_count_[4], class_count_[5], class_count_[6], class_count_[0], class_count_[7],
                            hud_cam_is_world_ ? " (the HUD camera is the world)" : "");
                    if (rt64_3ds::render_desc().primary_world_instance) {
                        // The last frame's candidates for the world: instance, group, records/triangles.
                        char buf[200];
                        int at = 0;
                        for (int k = 0; k < inst_table_n_ && k < kInstTable && at < (int)sizeof(buf) - 40; k++) {
                            const rt64_3ds::InstanceCount& c = inst_table_[k];
                            at += snprintf(buf + at, sizeof(buf) - at, " %u:%x %u/%u", c.inst, c.proj_id, c.records, c.tris);
                        }
                        buf[at] = 0;
                        fprintf(stderr, "rt64-3ds:   world instance %u of %d (instance:group records/triangles)%s%s; HUD camera the world in %u of %u frames drawing it; covering rects at the HUD's shift %u\n",
                                primary_inst_, inst_table_n_, buf, inst_table_n_ > kInstTable ? " ..." : "", hudcam_world_frames_, hudcam_frames_, covering_hud_);
                        covering_hud_ = hudcam_world_frames_ = hudcam_frames_ = 0;
                    }
                    fprintf(stderr, "rt64-3ds:   stereo %s: slider %.2f sep %d (%.3f) conv %.1f (manual %.1f%s) near %.1f hud %d%s aim %.1f, replay per frame left %.1f right %.1f ms "
                            "(the right eye from the left's commands in %u of %u frames)\n",
                            s3d_on_ ? "on" : "off", slider_, st.sep_slider, (float)st.sep_slider * 0.002f * slider_ * st.slider_gain,
                            applied_conv_, (float)st.convergence_hundredths * 0.2f, st.auto_convergence ? ", auto" : "", near_z_, st.hud_depth,
                            rt64_3ds::first_person_scene() ? " first-person" : "", aim_z_,
                            (float)acc_left_ * 1000.0f / SYSCLOCK_ARM11 / fw, (float)acc_right_ * 1000.0f / SYSCLOCK_ARM11 / fw,
                            eye_replays_, eye_frames_);
                    acc_left_ = acc_right_ = 0;
                    eye_replays_ = eye_frames_ = 0;
                }
                if (rt64_3ds::render_desc().sample_drawn_frame) {
                    const uint32_t fw = frames_ - frames_at_fc_;
                    const float per = fc_frames_ ? 1.0f / (float)fc_frames_ : 0.0f;
                    fprintf(stderr, "rt64-3ds:   frame copies: %u of %u frames sample themselves; per such frame %.1f draws sample the frame, %.1f draw into copies, %.1f drawn again into the frame copy (both eyes)\n",
                            fc_frames_, fw, (float)fc_samples_ * per, (float)fc_image_draws_ * per, (float)fc_mirrored_ * per);
                    fc_frames_ = fc_samples_ = fc_image_draws_ = fc_mirrored_ = 0;
                    frames_at_fc_ = frames_;
                }
                // Where the geometry lands: screen-space bounds of the last frame.
                float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f, minz = 1e9f, maxz = -1e9f;
                int behind = 0, tris = 0, rects = 0;
                for (const rt64_3ds::DrawRecord& d : frame_.draws) {
                    if (d.kind == rt64_3ds::DrawRecord::Tris) tris++; else rects++;
                    for (uint32_t i = d.first; i < d.first + d.count && i < frame_.verts.size(); i++) {
                        const rt64_3ds::Vtx3ds& v = frame_.verts[i];
                        if (v.w <= 0.0f) { behind++; continue; }
                        float x = v.x / v.w, y = v.y / v.w, z = v.z / v.w;
                        if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y;
                        if (z < minz) minz = z; if (z > maxz) maxz = z;
                    }
                }
                int textured = 0, untextured = 0;
                for (const rt64_3ds::DrawRecord& d : frame_.draws) { if (d.tex[0].valid) textured++; else untextured++; }
                {
                    // The GPU's fragment load: the screen area the eye's
                    // triangles cover (clamped to the screen, nothing behind
                    // the eye), in screens, and how much of it blends or
                    // samples a 32-bit texture.
                    const float l = -geo_.x0, r = (float)geo_.w + geo_.x0, b = (float)geo_.h, screen = (r - l) * b;
                    float all = 0.0f, blended = 0.0f, tex32 = 0.0f;
                    for (const rt64_3ds::DrawRecord& d : frame_.draws) {
                        if (d.rtt >= 0) continue;
                        float area = 0.0f;
                        for (uint32_t i = d.first; i + 2 < d.first + d.count && i + 2 < frame_.verts.size(); i += 3) {
                            const rt64_3ds::Vtx3ds* v = &frame_.verts[i];
                            if (v[0].w <= 0.0f || v[1].w <= 0.0f || v[2].w <= 0.0f) continue;
                            float px[3], py[3];
                            for (int k = 0; k < 3; k++) {
                                px[k] = std::min(std::max(v[k].x / v[k].w, l), r);
                                py[k] = std::min(std::max(v[k].y / v[k].w, 0.0f), b);
                            }
                            area += std::fabs((px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0])) * 0.5f;
                        }
                        all += area;
                        if (draw_blends(d)) blended += area;
                        if (d.tex[0].valid && !d.tex[0].snapshot && !d.tex[0].frame && d.tex[0].rtt < 0 &&
                            ((d.tex[0].fmt == 0 && d.tex[0].siz == 3) || d.tex[0].fmt == 3 || d.tex[0].fmt == 4 || (d.tex[0].fmt == 2 && d.tex[0].tlut_mode == 3)))
                            tex32 += area;
                    }
                    fprintf(stderr, "rt64-3ds:   fill %.2f screens an eye (%.2f blended, %.2f sampling RGBA32/I/IA textures)\n", all / screen, blended / screen, tex32 / screen);
                }
                fprintf(stderr, "rt64-3ds:   %d tri draws %d rect draws (%d textured, %d not, %u unresolved so far), x %.0f..%.0f y %.0f..%.0f z %.2f..%.2f, %d verts behind\n",
                        tris, rects, textured, untextured, is.tex_unresolved, minx, maxx, miny, maxy, minz, maxz, behind);
                if (!frame_.draws.empty()) {
                    const rt64_3ds::DrawRecord& d = frame_.draws[frame_.draws.size() / 2];
                    prof_[0] = prof_[1] = prof_[2] = prof_[3] = prof_[4] = 0;
                fprintf(stderr, "rt64-3ds:   mid draw: kind %d cc %06x %08x omh %06x oml %08x gm %06x tex0 %d %ux%u fmt %u/%u sc %d,%d-%d,%d\n",
                            (int)d.kind, d.cc_w0, d.cc_w1, d.othermode_h, d.othermode_l, d.geometry_mode, d.tex[0].valid,
                            d.tex[0].width, d.tex[0].height, d.tex[0].fmt, d.tex[0].siz, d.scissor[0], d.scissor[1], d.scissor[2], d.scissor[3]);
                }
            }
        }
    }

    // The frame cap (Settings::frame_cap). While the game asks for more than
    // 30 frames a second, a list that follows a drawn whole frame is run for
    // its state alone (Interpreter::pass_over, about a fifth of the cost) and
    // not drawn when it too is a whole frame on the screen alone: the screen
    // keeps the last frame, the game hears at once that its list is done.
    // Lists drawing off screen, copying the frame or into textures are always
    // drawn, and so is the list after a passed-over one.
    bool pass_next_ = false;           // the last list was drawn and a whole frame on the screen
    rt64_3ds::FrameRecord pass_frame_; // what a passed-over list's state commands record (nothing drawn)
    uint32_t passed_count_ = 0, pass_refused_ = 0, last_passed_ = 0, last_refused_ = 0;
    u64 acc_pass_ = 0;
    float pass_ms_ = 0.0f;
    bool pass_over(const OSTask* task) {
        const bool cap = rt64_3ds::settings().frame_cap == 1 && interp_.stats().refresh_rate > 30;
        if (!cap || !pass_next_) return false;
        pass_next_ = false;
        pass_frame_.clear();
        g_progress.phase = 1;
        if (interp_.pass_over(task->t.data_ptr, pass_frame_)) return true;
        pass_refused_++;
        return false;
    }

    // A list's read (send_dl): the interpreter's run over it and its
    // textures' decoding, up to "parsed" (tell_parsed), after which the game
    // may change anything the list points at.
    enum class ReadOutcome { PassedOver, Drawn, NotDrawn };
    ReadOutcome read_here(const OSTask* task) {
        if (pass_over(task)) return ReadOutcome::PassedOver;
        frame_.clear();
        vbo_idx_ ^= 1;
        frame_.verts.set_storage((rt64_3ds::Vtx3ds*)vbo_[vbo_idx_], kVboVerts);
        g_progress.phase = 1;
        maybe_capture(task);
        // In first person the reticle's quads must stay one per record.
        interp_.set_split_untagged_ortho(rt64_3ds::first_person_scene());
        const bool present = interp_.run(task->t.data_ptr, frame_);
        pass_next_ = present && interp_.last_list_plain();
        // Everything the replay reads from RDRAM is read by now once the
        // textures are decoded: let a game waiting on "parsed" (Banjo's
        // game_draw, which blocks on it every frame) go on before anything
        // else, the in-between and the wait for the presenter included.
        // Decoding needs no wait for the presenter: it allocates linear
        // memory and never touches the GX queue, and a texture rewritten in
        // place is retired (freed three frames later), never overwritten.
        const u64 tpf0 = svcGetSystemTick();
        prefetch_textures();
        acc_prefetch_ += svcGetSystemTick() - tpf0;
        // The reader's core wrote the vertices: flushed from it (the
        // replay's flush, on the gfx thread's core, covers what that core
        // writes). The textures flush themselves as they are decoded.
        if (reader_ != nullptr) flush_dcache(frame_.verts.data(), frame_.verts.size() * sizeof(GpuVertex));
        return present ? ReadOutcome::Drawn : ReadOutcome::NotDrawn;
    }
    void tell_parsed(uint32_t data_ptr, ReadOutcome read) {
        ultramodern::extensions::on_displaylist_parsed(data_ptr);
        // RenderDesc::early_drawn: and drawn, so a game that waits for that
        // before its next frame does not wait for the screen below too. With
        // depth_to_rdram, once the last game frame's depth is in RDRAM
        // (early_depth, send_dl): the game must not run while it is written.
        if (read == ReadOutcome::PassedOver || !early_drawn_) return;
        if (read == ReadOutcome::Drawn && rt64_3ds::render_desc().depth_to_rdram) drawn_after_depth_ = true;
        else ultramodern::renderer::report_list_drawn();
    }
    bool early_drawn_ = false;                  // RenderDesc::early_drawn, unless EARLY_DRAWN_OFF.TXT
    bool drawn_after_depth_ = false;            // tell_parsed left "drawn" to early_depth
    bool depth_read_early_ = false;             // this list's game frame has its depth grid (early_depth)

    // ---- the reader thread (Settings::read_core): a list's read on another
    // core while the gfx thread waits for it. For a game that waits on
    // "parsed" every frame with its own core idle meanwhile and faster than
    // this one's (two cores, the game on core 0, the renderer on core 1 with
    // the system's 20% and the synthesizer): Banjo's read took 26 ms on core
    // 1 against about 21 on core 0, and its frame is its own work plus the read.
    // The gfx thread does nothing while it waits, so the read touches what
    // it always did, one thread at a time; the presenter (core 1) runs
    // beside it as it did beside the gfx thread's read. READER_OFF.TXT keeps
    // the read on the gfx thread.
    // Its priority (Settings::read_priority) is just below the game's
    // threads: they go first while the game's main thread waits, as they
    // did beside the read on core 1. Above them, Banjo's audio thread waited
    // out each read (21 ms) and the game made 95% of its audio, underrunning
    // three times as often. The game's work and the read share core 0
    // either way. The gfx thread has the list before "parsed" goes out: the
    // game's main thread, woken by it, would otherwise hold the reader (and
    // the drawing on core 1) until its next wait.
    Thread reader_ = nullptr;
    LightEvent read_progress_;         // the reader published triangles, or is done
    float last_drain_ms_ = 0.0f;       // the list's triangles output during its read (read_list)
    bool defer_tris_ = false;          // the triangles' output left to the gfx thread
    bool defer_verts_ = false;         // ... and the vertices' transforms
    LightSemaphore read_go_, read_done_;
    volatile bool reader_stop_ = false;
    const OSTask* read_task_ = nullptr;
    ReadOutcome read_outcome_ = ReadOutcome::NotDrawn;
    static void reader_main(void* arg) {
        C3dRenderContext* self = static_cast<C3dRenderContext*>(arg);
        for (;;) {
            LightSemaphore_Acquire(&self->read_go_, 1);
            if (self->reader_stop_) break;
            __dmb();
            const uint32_t data_ptr = self->read_task_->t.data_ptr;
            const ReadOutcome read = self->read_here(self->read_task_);
            self->read_outcome_ = read;
            __dmb();
            LightSemaphore_Release(&self->read_done_, 1);
            LightEvent_Signal(&self->read_progress_);   // (a gfx thread draining meanwhile)
            self->tell_parsed(data_ptr, read);
        }
    }
    void start_reader() {
        const int core = rt64_3ds::settings().read_core;
        if (core < 0) return;
        if (FILE* f = rt64_3ds::data_fopen("READER_OFF.TXT", "r")) {
            fclose(f);
            fprintf(stderr, "rt64-3ds: list reader off (READER_OFF.TXT)\n");
            return;
        }
        LightSemaphore_Init(&read_go_, 0, 1);
        LightSemaphore_Init(&read_done_, 0, 1);
        // The gfx thread's stack; the priority the platform gives (else the
        // gfx thread's).
        s32 prio = 0x2C;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        if (rt64_3ds::settings().read_priority > 0) prio = rt64_3ds::settings().read_priority;
        reader_ = threadCreate(reader_main, this, 256 * 1024, prio, core, true);
        // The triangles' output (culling, texel coordinates, clipping, the
        // vertices: about a third of the read) and the vertices' transforms
        // after "parsed", here (Interpreter::set_defer_triangles).
        // TRIS_ON_READER.TXT keeps both in the read, VERTS_ON_READER.TXT the
        // transforms.
        if (reader_ != nullptr) {
            defer_tris_ = true;
            defer_verts_ = true;
            if (FILE* f = rt64_3ds::data_fopen("TRIS_ON_READER.TXT", "r")) { fclose(f); defer_tris_ = false; }
            if (FILE* f = rt64_3ds::data_fopen("VERTS_ON_READER.TXT", "r")) { fclose(f); defer_verts_ = false; }
            LightEvent_Init(&read_progress_, RESET_ONESHOT);
            interp_.set_defer_triangles(defer_tris_, defer_verts_, &read_progress, this);
        }
        fprintf(stderr, "rt64-3ds: list reader %s (priority 0x%lx, core %d), triangles' output %s, vertex transforms %s\n",
                reader_ != nullptr ? "up" : "FAILED: lists read on this thread", (unsigned long)prio, core,
                defer_tris_ ? "on the gfx thread" : "in the read", defer_tris_ && defer_verts_ ? "on the gfx thread" : "in the read");
    }
    // The reader published triangles (Interpreter::set_defer_triangles).
    static void read_progress(void* arg) { LightEvent_Signal(&static_cast<C3dRenderContext*>(arg)->read_progress_); }
    ReadOutcome read_list(const OSTask* task) {
        if (reader_ == nullptr) {
            const ReadOutcome read = read_here(task);
            tell_parsed(task->t.data_ptr, read);
            return read;
        }
        read_task_ = task;
        __dmb();
        LightSemaphore_Release(&read_go_, 1);
        last_drain_ms_ = 0.0f;
        if (defer_tris_) {
            // Meanwhile, the output of the triangles the reader has recorded
            // so far (Interpreter::drain_triangles), so little of it is left
            // once the read is done.
            u64 drained = 0;
            for (;;) {
                const u64 d0 = svcGetSystemTick();
                interp_.drain_triangles();
                drained += svcGetSystemTick() - d0;
                if (LightSemaphore_TryAcquire(&read_done_, 1) == 0) break;
                LightEvent_Wait(&read_progress_);
            }
            acc_tris_ += drained;
            last_drain_ms_ = (float)drained * 1000.0f / (float)SYSCLOCK_ARM11;
        }
        else LightSemaphore_Acquire(&read_done_, 1);
        __dmb();
        return read_outcome_;
    }
    void stop_reader() {
        if (reader_ == nullptr) return;
        reader_stop_ = true;
        __dmb();
        LightSemaphore_Release(&read_go_, 1);
        threadJoin(reader_, 1000000000ull);
        reader_ = nullptr;
    }

private:
    void init() {
        if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2)) {
            fprintf(stderr, "rt64-3ds: C3D_Init failed\n");
            return;
        }
        LightLock_Init(&g_gpu_lock);
        aptHook(&g_apt_cookie, apt_hook, nullptr);
        textures_.set_compact_intensity(rt64_3ds::settings().gpu_textures16 != 0);
        // Textures in VRAM (TextureCache::set_vram); TEX_VRAM_OFF.TXT keeps
        // them in the linear heap.
        {
            bool vram = true;
            if (FILE* f = rt64_3ds::data_fopen("TEX_VRAM_OFF.TXT", "r")) { fclose(f); vram = false; }
            // Off-screen images' targets (colour and 24-bit depth) are the VRAM
            // made later, on demand: render_to_texture (Banjo's jiggy
            // transition, 512x256: 1 MB) and sample_drawn_frame (Goemon's menu
            // rims, 256x256: 512 KB). Everything else is made at start.
            const rt64_3ds::RenderDesc& rd = rt64_3ds::render_desc();
            const uint32_t reserve = rd.render_to_texture ? 1536u * 1024 : rd.sample_drawn_frame ? 640u * 1024 : 128u * 1024;
            textures_.set_vram(vram, reserve);
            fprintf(stderr, "rt64-3ds: textures in %s, %u KB kept free\n", vram ? "VRAM (as room allows)" : "the linear heap (TEX_VRAM_OFF.TXT)", (unsigned)(reserve / 1024));
        }
        // "Drawn" as soon as the list is read (RenderDesc::early_drawn);
        // EARLY_DRAWN_OFF.TXT: once the renderer is done with it, for A/B.
        early_drawn_ = rt64_3ds::render_desc().early_drawn;
        if (early_drawn_) {
            if (FILE* f = rt64_3ds::data_fopen("EARLY_DRAWN_OFF.TXT", "r")) {
                fclose(f);
                early_drawn_ = false;
                fprintf(stderr, "rt64-3ds: lists reported drawn once done with (EARLY_DRAWN_OFF.TXT)\n");
            }
        }
        // In-between and game-frame checks of the flat draws (the log names
        // one that jumped for a frame).
        if (FILE* f = rt64_3ds::data_fopen("INTERP_CHECK.TXT", "r")) {
            fclose(f);
            interp_check_ = true;
            fprintf(stderr, "rt64-3ds: flat draws checked against their frames (INTERP_CHECK.TXT)\n");
        }
        // The right eye from the left eye's commands (replay_and_present).
        if (FILE* f = rt64_3ds::data_fopen("EYE_REPLAY_OFF.TXT", "r")) {
            fclose(f);
            eye_replay_on_ = false;
            fprintf(stderr, "rt64-3ds: both eyes' draws recorded (EYE_REPLAY_OFF.TXT)\n");
        }
        // The game's own mipmap levels (TexDesc::levels); MIPMAP_OFF.TXT
        // draws the base level alone.
        if (FILE* f = rt64_3ds::data_fopen("MIPMAP_OFF.TXT", "r")) {
            fclose(f);
            textures_.set_mipmaps(false);
            fprintf(stderr, "rt64-3ds: mipmaps off (MIPMAP_OFF.TXT)\n");
        }
        fill_clear_ = rt64_3ds::settings().gpu_fill_clear != 0;
        // The eyes' formats (RenderDesc::eye_color16 / eye_depth16 through
        // Settings::gpu_color16 / gpu_depth16).
        color16_ = rt64_3ds::settings().gpu_color16 != 0;
        depth16_ = rt64_3ds::settings().gpu_depth16 != 0;
        const GPU_COLORBUF eye_color = color16_ ? GPU_RB_RGB565 : GPU_RB_RGBA8;
        const GPU_DEPTHBUF eye_depth = depth16_ ? GPU_RB_DEPTH16 : GPU_RB_DEPTH24_STENCIL8;
        transfer_flags_ = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                          GX_TRANSFER_IN_FORMAT(color16_ ? GX_TRANSFER_FMT_RGB565 : GX_TRANSFER_FMT_RGBA8) |
                          GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
        // Clears fill memory with the value as is: 16 bits of it for RGB565.
        eye_clear_ = color16_ ? 0x0000u : 0x000000FFu;
        top_ = C3D_RenderTargetCreate(240, 400, eye_color, eye_depth);
        if (top_ == nullptr) {
            fprintf(stderr, "rt64-3ds: render target creation failed\n");
            return;
        }
        C3D_RenderTargetSetOutput(top_, GFX_TOP, GFX_LEFT, transfer_flags_);
        // The right eye, drawn only while the 3D slider is up.
        // What the screen shows while the game has the video blanked (below).
        black_ = C3D_RenderTargetCreate(240, 400, eye_color, C3D_DEPTHTYPE(-1));
        top_r_ = C3D_RenderTargetCreate(240, 400, eye_color, eye_depth);
        if (top_r_ != nullptr) C3D_RenderTargetSetOutput(top_r_, GFX_TOP, GFX_RIGHT, transfer_flags_);
        else fprintf(stderr, "rt64-3ds: no right-eye target (VRAM): stereo off\n");
        cur_eye_[0] = last_eye_[0] = top_;
        cur_eye_[1] = last_eye_[1] = top_r_;
        fprintf(stderr, "rt64-3ds: eye targets: %s colour, %s depth (VRAM left %u KB)\n", color16_ ? "RGB565" : "RGBA8",
                depth16_ ? "16-bit" : "24-bit + stencil", (unsigned)(vramSpaceFree() / 1024));
        // The game frame's depth, copied by the GPU after its left eye's draws
        // (queue_depth_copy), for the depth reads (read_depth_zp): the CPU
        // reads VRAM slowly (the grid's 1200 texels took about a millisecond
        // a frame on the console). DEPTH_COPY_OFF.TXT reads VRAM as before.
        if (FILE* f = rt64_3ds::data_fopen("DEPTH_COPY_OFF.TXT", "r")) fclose(f);
        else {
            depth_copy_bytes_ = C3D_CalcDepthBufSize(240, 400, eye_depth);
            depth_copy_ = (u8*)linearAlloc(depth_copy_bytes_);
        }
        fprintf(stderr, "rt64-3ds: depth read %s\n", depth_copy_ != nullptr ? "from a GPU copy in linear memory" : "from VRAM");
        gfxSet3D(false);

        dvlb_ = DVLB_ParseFile((u32*)rt64_3ds_shbin, rt64_3ds_shbin_size);
        shaderProgramInit(&prog_);
        shaderProgramSetVsh(&prog_, &dvlb_->DVLE[0]);
        C3D_BindProgram(&prog_);
        u_xform_ = shaderInstanceGetUniformLocation(prog_.vertexShader, "xform");
        u_stereo_ = shaderInstanceGetUniformLocation(prog_.vertexShader, "stereo");
        u_eye_ = shaderInstanceGetUniformLocation(prog_.vertexShader, "eye");
        u_uvscale_ = shaderInstanceGetUniformLocation(prog_.vertexShader, "uvscale");
        u_uv1_ = shaderInstanceGetUniformLocation(prog_.vertexShader, "uv1");

        C3D_AttrInfo* ai = C3D_GetAttrInfo();
        AttrInfo_Init(ai);
        AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);
        AttrInfo_AddLoader(ai, 1, GPU_FLOAT, 2);
        AttrInfo_AddLoader(ai, 2, GPU_UNSIGNED_BYTE, 4);

        for (int i = 0; i < 2; i++) vbo_[i] = (GpuVertex*)linearAlloc(kVboVerts * sizeof(GpuVertex));
        // Frame interpolation: the in-between frames' vertices (the GPU may
        // still read the game frame's last buffer while both are recorded).
        // Two: a game frame of three or four refreshes has two or three
        // in-betweens, each built while the one before is on the GPU.
        if (rt64_3ds::render_desc().frame_interpolation) {
            vbo_[2] = (GpuVertex*)linearAlloc(kVboVerts * sizeof(GpuVertex));
            vbo_[3] = (GpuVertex*)linearAlloc(kVboVerts * sizeof(GpuVertex));
            start_presenter();
        }
        LightLock_Init(&g_readback_lock);
        for (int i = 0; i < 2; i++) g_readback[i] = (u16*)linearAlloc(240 * 400 * sizeof(u16));
        if (vbo_[0] == nullptr || vbo_[1] == nullptr) {
            fprintf(stderr, "rt64-3ds: vertex buffer allocation failed\n");
            return;
        }

        // A white 8x8 texture for any unit a combiner samples without a
        // resolved texture: the PICA hangs on an unconfigured texture unit
        // (the emulator does not).
        if (C3D_TexInit(&white_, 8, 8, GPU_RGBA5551)) {
            memset(white_.data, 0xFF, 8 * 8 * 2);
            flush_dcache(white_.data, 8 * 8 * 2);
            C3D_TexSetFilter(&white_, GPU_NEAREST, GPU_NEAREST);
        }

        // The snapshot texture: the 240x400 colour buffer copied whole into
        // a 256x512 RGBA8 texture (same tiling, rows padded), for the game's
        // framebuffer effects.
        // (In the eyes' format: the copy is byte for byte.)
        const GPU_TEXCOLOR snap_fmt = color16_ ? GPU_RGB565 : GPU_RGBA8;
        if (!C3D_TexInitVRAM(&snapshot_, 256, 512, snap_fmt)) {
            if (!C3D_TexInit(&snapshot_, 256, 512, snap_fmt)) fprintf(stderr, "rt64-3ds: snapshot texture allocation failed\n");
        }
        C3D_TexSetFilter(&snapshot_, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&snapshot_, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        if (rt64_3ds::render_desc().sample_drawn_frame) {
            // RenderDesc::sample_drawn_frame: the copy of an eye's frame drawn
            // so far, as a render target with its own depth buffer. It holds
            // the eye's 240x400 where the snapshot would (kFrameRowOffset).
            if (C3D_TexInitVRAM(&frame_tex_, 256, 512, GPU_RGBA8)) {
                frame_rt_ = C3D_RenderTargetCreateFromTex(&frame_tex_, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
                if (frame_rt_ == nullptr) C3D_TexDelete(&frame_tex_);
            }
            if (frame_rt_ != nullptr) {
                // Copies are texel for texel (the RDP's copy mode).
                C3D_TexSetFilter(&frame_tex_, GPU_NEAREST, GPU_NEAREST);
                C3D_TexSetWrap(&frame_tex_, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            }
            fprintf(stderr, "rt64-3ds: frame copy target %s (VRAM free %u KB)\n", frame_rt_ != nullptr ? "ready" : "FAILED: frames that sample themselves draw from white",
                    (unsigned)(vramSpaceFree() / 1024));
        }
        if (rt64_3ds::render_desc().frame_interpolation && rt64_3ds::settings().gpu_double_eyes != 0) init_double_eyes(eye_color, eye_depth);
        {
            // The game's frame on the screen, as RT64 would place it (see
            // rt64_3ds::ScreenGeometry).
            geo_ = rt64_3ds::screen_geometry();
            grid_cols_ = std::min((geo_.w + 7) / 8, kGridColsMax);
            grid_rows_ = std::min((geo_.h + 7) / 8, kGridRowsMax);
            interp_.set_screen(geo_);
            fprintf(stderr, "rt64-3ds: game frame %dx%d, x%.3f, %.1f-unit margins\n", geo_.w, geo_.h, geo_.scale, geo_.x0);
        }
        {
            // The perspective groups the game's rules place as world: their
            // projection turns the depth buffer into distances.
            const rt64_3ds::RenderDesc& desc = rt64_3ds::render_desc();
            uint32_t ids[4];
            int n = 0;
            for (size_t i = 0; i < desc.rule_count && n < 4; i++) {
                const rt64_3ds::StereoRule& r = desc.rules[i];
                if (r.cls == rt64_3ds::StereoClass::World && (r.kind == rt64_3ds::ProjKind::Any || r.kind == rt64_3ds::ProjKind::Perspective) &&
                    r.id_lo == r.id_hi) ids[n++] = r.id_lo;
            }
            if (n == 0) ids[n++] = desc.frame_head_proj_id;
            interp_.set_world_proj_ids(ids, n);
            interp_.set_force_branch_z(desc.force_lod_branch);
            interp_.set_gbi(desc.gbi);
            interp_.set_render_to_texture(desc.render_to_texture);
            interp_.set_sample_drawn_frame(desc.sample_drawn_frame);
            interp_.set_primary_world_instance(desc.primary_world_instance);
            interp_.set_unify_world_depth(desc.unify_world_depth);
            interp_.set_fix_rect_lr(desc.fix_rect_lr);
            interp_.set_merge_rect_slices(desc.merge_rect_slices);
            interp_.set_hud_camera_id(desc.hud_camera_id);
            interp_.set_merge_image_tiles(desc.merge_image_tiles);
        }

        // The game's frame sits centred on the 400 px wide screen, scaled to
        // its height (DK64's 320x240: x -40..360). Everything arrives in N64
        // screen pixels with y growing downward: bottom edge = h, top = 0.
        Mtx_OrthoTilt(&proj_, -geo_.x0, (float)geo_.w + geo_.x0, (float)geo_.h, 0.0f, 0.0f, 1.0f, true);
        C3D_DepthMap(true, -1.0f, 0.0f);
        C3D_CullFace(GPU_CULL_NONE);
        start_reader();
        ok_ = true;
        // GPU_DEBUG.TXT: "<start frame> <frame count> <sync 0/1> <every N frames>" (defaults 0, all, 1, 1).
        if (FILE* f = rt64_3ds::data_fopen("GPU_DEBUG.TXT", "r")) {
            int a = 0, b = 1000000000, c = 1, d = 1;
            if (fscanf(f, "%d %d %d %d", &a, &b, &c, &d) < 1) { a = 0; }
            fclose(f);
            gpu_debug_ = true;
            debug_from_ = (uint32_t)a; debug_count_ = (uint32_t)b; debug_sync_ = c != 0; debug_step_ = d > 0 ? (uint32_t)d : 1;
            fprintf(stderr, "rt64-3ds: GPU debug mode from frame %u for %u frames, sync %d, every %u\n", debug_from_, debug_count_, (int)debug_sync_, debug_step_);
        }
        if (FILE* f = rt64_3ds::data_fopen("PROFILE.TXT", "r")) { prof_on_ = true; fclose(f); }
        interp_.set_profile(prof_on_);
        // DRAW_RANGE.TXT: "<first> <last>": replay only those draws of every
        // frame (to find which draw goes wrong against the host reference).
        if (FILE* f = rt64_3ds::data_fopen("DRAW_RANGE.TXT", "r")) {
            if (fscanf(f, "%d %d", &draw_lo_, &draw_hi_) != 2) { draw_lo_ = 0; draw_hi_ = 1 << 30; }
            fclose(f);
            fprintf(stderr, "rt64-3ds: replaying draws %d..%d only\n", draw_lo_, draw_hi_);
        }
        fprintf(stderr, "rt64-3ds: citro3d renderer up (linear free %u KB)\n", (unsigned)(linearSpaceFree() / 1024));
    }

    // How a frame is presented. Normal: as soon as it is drawn (a frame still
    // waiting for its refresh is replaced). Frame interpolation, while the
    // game asks for less than 60: Inbetween, the interpolated frame, and
    // Paced, the game's own; each waits until the frame before it has been
    // shown, so every one is on the screen for a refresh.
    enum class Present { Normal, Inbetween, Paced };
    // handoff: the presenter thread waits for the screen and ends the frame
    // (then releases g_gpu_lock); returns whether it did.
    // An in-between with double eyes (its eye targets are its own) goes to
    // the GPU as soon as it is recorded, without its display transfer:
    // only the transfer waits for the screen (transfer_inbetween), so its
    // draws run while the gfx thread records the game's frame and while
    // the frame before it is still waiting for its refresh.
    // hold (the last in-between of a list): the presenter, not this
    // thread, waits for the screen and transfers it, then presents the
    // game's frame (recorded on its own: see present_deferred), so the
    // list is done a refresh sooner. A game frame that can't be recorded
    // on its own has it transferred here first (flush_pending_inbetween).
    bool replay_and_present(Present kind = Present::Normal, bool handoff = false, bool hold = false) {
        const bool inbetween = kind == Present::Inbetween;
        await_presenter();
        // GFX_CAPTURE.TXT "seq": every frame put on the screen.
        if (g_seq_request > 0) { seq_first_ = (int)frames_; seq_count_ = g_seq_request; g_seq_request = 0; }
        if (seq_count_ > 0 && (int)frames_ >= seq_first_ && (int)frames_ < seq_first_ + seq_count_) fb_capture_pending_ = true;
        last_draws_ = frame_.draws.size();
        last_verts_ = frame_.verts.size();
        frames_++;
        g_progress.frames = frames_;
        g_progress.draw_count = (uint32_t)frame_.draws.size();
        g_progress.draw_index = 0;
        g_progress.phase = 2;
        bool debug_frame = gpu_debug_ && frames_ >= debug_from_ && frames_ < debug_from_ + debug_count_ && ((frames_ - debug_from_) % debug_step_) == 0;
        if (debug_frame) {
            fprintf(stderr, "rt64-3ds: === frame %u: %u draws, cimg %06x\n", frames_, (unsigned)frame_.draws.size(), frame_.color_image);
            // Every draw of the frame goes to the log before the GPU sees it,
            // with its first vertices.
            for (size_t i = 0; i < frame_.draws.size(); i++) {
                const rt64_3ds::DrawRecord& d = frame_.draws[i];
                describe_draw((uint32_t)i, d);
                for (uint32_t k = d.first; k < d.first + 4 && k < d.first + d.count && k < frame_.verts.size(); k++) {
                    const rt64_3ds::Vtx3ds& v = frame_.verts[k];
                    fprintf(stderr, "rt64-3ds:    v%u x %.1f y %.1f z %.3f w %.3f u %.2f v %.2f rgba %02x%02x%02x%02x\n", k - d.first,
                            v.x / v.w, v.y / v.w, v.z / v.w, v.w, v.u, v.v, v.r, v.g, v.b, v.a);
                }
            }
        }
        // RenderDesc::double_eyes: the in-between draws into eye targets of
        // its own, and the game's frame of the pair (handed to the
        // presenter) is recorded into a command buffer of its own without
        // a C3D_FrameBegin, which would wait for all of the in-between's GPU
        // work (see present_deferred). Whatever needs the GPU idle at a
        // frame's start (copies out of the last frame, the capture and
        // screenshot reads, a change of the screen's output, the GPU debug
        // mode) keeps a frame begun as ever.
        // record_only_ (send_dl, with the builder): recorded on its own
        // whatever comes up meanwhile (it was checked before the build began).
        const bool defer = record_only_ || (double_eyes_ && pair_ && kind == Present::Paced && handoff && !debug_frame && !fb_capture_pending_ &&
                                            !g_shot_request && !frame_.snapshot_request && !vi_blanked_);
        if (inb_pending_ && !defer) flush_pending_inbetween();
        const int set = (double_eyes_ && inbetween) ? 1 : 0;
        cur_eye_[0] = set != 0 ? inb_l_ : top_;
        cur_eye_[1] = set != 0 ? inb_r_ : top_r_;
        if (pair_ && !inbetween) pair_frames_++;
        if (defer) begin_deferred();
        else {
            if (frame_.snapshot_request && snapshot_.data != nullptr) copy_snapshot();
            const bool readback = start_readback();
            update_vi_blank();
            u64 tb0 = svcGetSystemTick();
            retire_rtt_targets();
            // Textures' copies to VRAM, ahead of this frame's draws (out of a
            // frame: see TextureCache::upload_pending).
            textures_.upload_pending();
            C3D_FrameBegin(0);
            const u64 waited = svcGetSystemTick() - tb0;
            acc_wait_ += waited;
            // An interpolated pair's frames: what each waited for the GPU
            // work before it (the in-between for the game's frame before,
            // the game's frame for its in-between).
            if (pair_) acc_pair_wait_[inbetween ? 1 : 0] += waited;
            // The frame ended last (here or by the presenter) is done:
            // citro3d timed its GPU work from C3D_FrameEnd's start of the
            // queue to its last command (clears, draws, ghost passes and
            // the display transfers to the screen); a frame the presenter
            // drew in two (double eyes) adds its draws.
            account_gpu(gpu_pending_kind_, gpu_pending_eyes_, gpu_pending_fill_, C3D_GetDrawingTime() + gpu_pending_extra_ms_);
            gpu_pending_kind_ = -1;
            gpu_pending_extra_ms_ = 0.0f;
            if (readback) finish_readback();
            if (check_snapshot_) { verify_snapshot(); check_snapshot_ = false; }
            maybe_capture_framebuffer();
            maybe_screenshot();
            link_eyes(set);
        }
        const rt64_3ds::StereoFrame sf = begin_stereo_frame();
        {
            // The finished frame's depth, read once on a grid: the game's
            // RDRAM depth buffer and the stereo near statistic both use it.
            u64 td = svcGetSystemTick();
            const bool rdram = rt64_3ds::render_desc().depth_to_rdram && prev_depth_image_ != 0 && !inbetween;
            if (!inbetween && depth_read_early_) depth_read_early_ = false;     // read before "drawn" (early_depth)
            else if (!inbetween && (rdram || stereo_wants_depth_)) read_game_depth(rdram);
            acc_depth_ += svcGetSystemTick() - td;
        }
        const rt64_3ds::StereoFrame sfd = finish_stereo_frame(sf, inbetween);
        classify_frame(sfd);
        if (interp_check_) {
            if (inbetween) check_inbetween_flat();
            else note_flat_boxes();
        }
        g_progress.phase = 3;
        for (const rt64_3ds::DrawRecord& d : frame_.draws) {
            if (d.tex[0].valid && d.tex[0].snapshot) {
                if (capture_snapshot_frames_ > 0 && !fb_capture_pending_) { capture_snapshot_frames_--; fb_capture_pending_ = true; }
                // Ask the emulator harness for a screenshot of this frame
                // (rate-limited: one per second).
                static u64 last = 0;
                u64 now = svcGetSystemTick();
                if (now - last > SYSCLOCK_ARM11) { last = now; fprintf(stderr, "AUTOTEST shot snapshot-draw\n"); }
                break;
            }
        }
        // A frame that ends on an off-screen image draws only for that image
        // (Banjo's jiggy transition renders the scene into its own image
        // and presents nothing): fill the texture, leave the screen with the
        // previous frame, as the VI would.
        bool offscreen_frame = false;
        for (const rt64_3ds::RttImage& img : frame_.rtts) if (img.addr != 0 && img.addr == frame_.color_image) offscreen_frame = true;
        // RenderDesc::fill_clear: the frame's leading full-screen fill
        // rectangle becomes the eyes' clear value, and is not drawn.
        u32 clear_value = eye_clear_;
        const int fill_skip = (!offscreen_frame && fill_clear_) ? leading_fill(sfd, &clear_value) : -1;
        if (!offscreen_frame) {
            clear_target(cur_eye_[0], C3D_CLEAR_ALL, clear_value);
            draw_on(cur_eye_[0]);
        }
        cur_target_ = offscreen_frame ? -2 : -1;
        frame_viewport_ = false;
        C3D_BindProgram(&prog_);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_xform_, &proj_);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo_, 0.0f, 0.0f, 0.0f, 0.0f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, u_eye_, 1.0f, 0.0f, 0.0f, 0.0f);
        last_sa_ = last_sb_ = 0.0f;
        C3D_TexBind(0, &white_);
        C3D_TexBind(1, &white_);
        C3D_TexBind(2, &white_);
        last_tex_[0] = last_tex_[1] = &white_;
        last_tex_param_[0] = last_tex_param_[1] = 0xFFFFFFFFu;
        last_cc_[0] = last_cc_[1] = 0xFFFFFFFFu;
        misc_valid_ = false;
        last_us_ = last_vs_ = -1.0f;
        last_uv1_[0] = -1.0f;
        last_plan_.stages = -1;

        // The interpreter wrote the frame's vertices straight into this
        // frame's linear buffer (the other one may still be read by the GPU).
        GpuVertex* vb = frame_.verts.data();
        size_t n = frame_.verts.size();
        // A full-screen quad after the frame's vertices, for the ghost pass.
        const bool ghost = n + 6 <= kVboVerts && write_ghost_quad(vb + n);
        // The kernel's flush, on this core: GSPGPU_FlushDataCache is a
        // request to the GSP module on core 1, which waits for that core.
        flush_dcache(vb, (n + (ghost ? 6 : 0)) * sizeof(GpuVertex));
        C3D_BufInfo* bi = C3D_GetBufInfo();
        BufInfo_Init(bi);
        BufInfo_Add(bi, vb, sizeof(GpuVertex), 3, 0x210);

        int fallbacks = 0;
        u64 tl0 = svcGetSystemTick(), tl_split = 0;
        const int eyes = (sfd.on && !offscreen_frame) ? 2 : 1;
        // RenderDesc::sample_drawn_frame: this frame copies parts of itself
        // mid-frame (see replay_split below).
        const bool split = frame_.frame_sample_last >= 0 && frame_rt_ != nullptr && !offscreen_frame;
        if (split) fc_frames_++;
        // The right eye from the left eye's commands: the left eye's draws
        // are a command list of their own (they set all their state
        // themselves, from caches emptied at its start), queued a second time
        // after the right eye's target and sign. Not for frames that draw
        // into other targets between the eyes' draws (off-screen images, the
        // frame copy), nor in the GPU debug mode. EYE_REPLAY_OFF.TXT: both
        // eyes recorded, as before.
        bool eye_replay = eyes == 2 && eye_replay_on_ && !split && !(debug_frame && debug_sync_);
        if (eye_replay) for (const rt64_3ds::DrawRecord& d : frame_.draws) if (d.rtt >= 0) { eye_replay = false; break; }
        u32* left_list = nullptr;
        u32 left_words = 0;
        if (eyes == 2) eye_frames_++;
        for (int pass = 0; pass < eyes; pass++) {
        const int eye = pass == 0 ? 1 : -1;       // +1 left, -1 right
        if (pass == 1) {
            // The right eye: same records, same state, the other shift.
            tl_split = svcGetSystemTick();
            clear_target(cur_eye_[1], C3D_CLEAR_ALL, clear_value);
            draw_on(cur_eye_[1]);
            cur_target_ = -1;
            frame_viewport_ = false;
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_xform_, &proj_);
        }
        C3D_FVUnifSet(GPU_VERTEX_SHADER, u_eye_, (float)eye, 0.0f, 0.0f, 0.0f);
        if (pass == 1 && left_list != nullptr) {
            // The right eye's target and sign to the GPU, then the left
            // eye's draws again. They leave the GPU as they left it after the
            // left eye, which is what citro3d holds apart from the target and
            // the sign (the ghost pass, between them, sets all it uses).
            C3Di_UpdateContext();
            close_segment();
            queue_again(left_list, left_words);
            eye_replays_++;
            forget_draw_state();
            if (ghost && sfd.on && !offscreen_frame) ghost_pass((uint32_t)n);
            continue;
        }
        if (eye_replay) {
            // The frame's setup and the left target, apart from the draws.
            C3Di_UpdateContext();
            close_segment();
            forget_draw_state();
        }
        // One record into one target (-1 the eye, kFrameTarget the copy of
        // the eye's frame, >= 0 an off-screen image).
        auto draw_rec = [&](size_t i, int tgt) {
            const rt64_3ds::DrawRecord& d = frame_.draws[i];
            g_progress.draw_index = (uint32_t)i;
            g_progress_draw = &d;
            if (tgt != cur_target_) {
                if (!bind_target(tgt, pass)) return;
                // A target that is also a texture was drawn into: bind
                // textures again (that clears the GPU's texture cache).
                if (split) last_tex_[0] = last_tex_[1] = nullptr;
            }
            apply_state(d, i, (pass == 0 && tgt != kFrameTarget) ? &fallbacks : nullptr);
            {
                // The left eye's shift: the shader multiplies it by the
                // eye's sign (u_eye_), so a draw's commands are the same for
                // both eyes (every class's shift is odd in the eye).
                float sa = 0.0f, sb = 0.0f;
                if (sfd.on && d.rtt < 0) {
                    if (classes_[i] == kReticle) sa = -sfd.aim_ndc * 200.0f;
                    else rt64_3ds::stereo_shift(sfd, (rt64_3ds::StereoClass)classes_[i], d, 1, &sa, &sb);
                }
                if (sa != last_sa_ || sb != last_sb_) {
                    C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo_, sa, sb, 0.0f, 0.0f);
                    last_sa_ = sa; last_sb_ = sb;
                }
            }
            u64 td = tick();
            C3Di_UpdateContext();          // the state flush, timed apart from the draw itself
            u64 tf = tick();
            prof_[4] += tf - td;
            C3D_DrawArrays(GPU_TRIANGLES, d.first, d.count);
            prof_[3] += tick() - tf;
            if (debug_frame && debug_sync_) {
                // Finish the frame here and start it again: FrameBegin waits
                // for the GPU, so a hang stops with draw_index naming the
                // draw that caused it (phase 2 = waiting after that draw).
                g_progress.phase = 4;
                C3D_FrameEnd(0);
                g_progress.phase = 2;
                C3D_FrameBegin(0);
                const int t = cur_target_;
                cur_target_ = -4;
                bind_target(t, pass);
                g_progress.phase = 3;
                C3D_BindProgram(&prog_);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo_, last_sa_, last_sb_, 0.0f, 0.0f);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, u_eye_, (float)eye, 0.0f, 0.0f, 0.0f);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, u_uvscale_, last_us_, last_vs_, 0.0f, 0.0f);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, u_uv1_, last_uv1_[0], last_uv1_[1], last_uv1_[2], last_uv1_[3]);
                bi = C3D_GetBufInfo();
                BufInfo_Init(bi);
                BufInfo_Add(bi, vb, sizeof(GpuVertex), 3, 0x210);
            }
        };
        auto replayable = [&](size_t i) {
            const rt64_3ds::DrawRecord& d = frame_.draws[i];
            return d.count >= 3 && d.first + d.count <= n && (int)i >= draw_lo_ && (int)i <= draw_hi_;
        };
        if (!split) {
            for (size_t i = 0; i < frame_.draws.size(); i++) {
                const rt64_3ds::DrawRecord& d = frame_.draws[i];
                if (!replayable(i)) continue;
                // Off-screen images are drawn once (in the first pass, unshifted).
                if (d.rtt >= 0 && pass != 0) continue;
                if (offscreen_frame && d.rtt < 0) continue;
                if ((int)i == fill_skip) continue;     // the eyes were cleared to it
                draw_rec(i, d.rtt);
            }
        }
        else {
            // The frame samples itself (TexDesc::frame): each eye keeps a copy
            // of its frame in frame_tex_ (the snapshot's layout) by drawing
            // its draws up to the last such sample there too, run by run, so
            // the copy holds what the eye holds wherever a draw samples it.
            // The off-screen images those draws fill hold this eye's copies,
            // so they are drawn in every pass, each eye's sequence before the
            // other's (the GPU runs it in order). The eye itself is drawn as
            // ever: its depth and everything read from it stay as they were.
            if (pass == 1) {
                // After the left eye's commands, not before them (a clear in
                // a frame goes straight onto the GX queue).
                frame_split();
            }
            clear_target(frame_rt_, C3D_CLEAR_ALL, 0x000000FF);
            const size_t count = frame_.draws.size();
            const size_t last = (size_t)frame_.frame_sample_last;
            auto samples = [&](const rt64_3ds::DrawRecord& d) {
                return (d.tex[0].valid && d.tex[0].frame) || (d.tex[1].valid && d.tex[1].frame);
            };
            for (size_t i = 0; i < count;) {
                const rt64_3ds::DrawRecord& d = frame_.draws[i];
                if (d.rtt >= 0 || samples(d)) {
                    // Into an image, or onto the eye from the copy (never into
                    // the copy: it would read itself).
                    if (replayable(i)) {
                        draw_rec(i, d.rtt);
                        if (samples(d)) fc_samples_++;
                        if (d.rtt >= 0) fc_image_draws_++;
                    }
                    i++;
                    continue;
                }
                size_t j = i;
                while (j < count && frame_.draws[j].rtt < 0 && !samples(frame_.draws[j])) j++;
                for (size_t k = i; k < j; k++) if (replayable(k)) draw_rec(k, -1);
                for (size_t k = i; k < j && k < last; k++) {
                    if (replayable(k)) { draw_rec(k, kFrameTarget); fc_mirrored_++; }
                }
                i = j;
            }
        }
        if (cur_target_ != -1 && !offscreen_frame) {
            bind_target(-1, pass);
        }
        // The left eye's draws, a command list kept for the right eye.
        if (eye_replay && pass == 0) close_segment(&left_list, &left_words);
        // A game frame's depth, copied once its left eye's draws are queued
        // (the next game frame reads it).
        if (pass == 0 && !inbetween) {
            depth_copy_ready_ = depth_copy_ != nullptr && !offscreen_frame;
            if (depth_copy_ready_) {
                if (!eye_replay) close_segment();
                queue_depth_copy();
            }
        }
        if (ghost && sfd.on && !offscreen_frame) ghost_pass((uint32_t)n);
        }   // eye passes
        {
            u64 tl1 = svcGetSystemTick();
            if (tl_split != 0) { acc_left_ += tl_split - tl0; acc_right_ += tl1 - tl_split; }
            else acc_left_ += tl1 - tl0;
        }
        if (vi_blanked_ && black_ != nullptr && !offscreen_frame) {
            // The frame is drawn (the game may store it for an effect) but
            // the screen gets the black target, as the VI would show.
            clear_target(black_, C3D_CLEAR_COLOR, eye_clear_);
            draw_on(black_);
        }
        g_progress.phase = 4;
        g_stats.combiner_fallbacks = fallbacks;
        g_stats.draws = (int)frame_.draws.size();
        g_stats.tris = (int)(frame_.verts.size() / 3);
        presented_++;
        if (!offscreen_frame) { last_eye_[0] = cur_eye_[0]; last_eye_[1] = cur_eye_[1]; }
        if (record_only_) {
            // The in-betweens are drawn next; hand_off_recorded() does this
            // frame's bookkeeping and hands it over after them.
            end_deferred();
            rec_sfd_ = sfd;
            rec_eyes_ = eyes;
            rec_fill_ = fill_skip >= 0;
            rec_eye_[0] = last_eye_[0];
            rec_eye_[1] = last_eye_[1];
            return false;
        }
        if (defer) {
            end_deferred();
            // The in-between's GPU time is the presenter's to read now (no
            // frame begins here); this frame's is read as ever at the next
            // frame begin, its draws (a frame of their own) added then.
            dinb_kind_ = gpu_pending_kind_;
            dinb_eyes_ = gpu_pending_eyes_;
            dinb_fill_ = gpu_pending_fill_;
        }
        gpu_pending_kind_ = inbetween ? 1 : 0;
        gpu_pending_eyes_ = eyes;
        gpu_pending_fill_ = fill_skip >= 0;
        if (handoff) {
            // What follows the frame's end is bookkeeping of this thread:
            // done first, then the open (or recorded) frame goes to the
            // presenter.
            finish_frame_bookkeeping(inbetween, sfd);
            job_deferred_ = defer;
            job_stereo_ = eyes == 2;
            job_inb_ = inb_pending_;    // transferred by the presenter, ahead of this frame
            job_inb_stereo_ = inb_stereo_;
            inb_pending_ = false;
            if (defer) {
                LightEvent_Clear(&inb_free_);
                deferred_frames_++;
            }
            present_busy_ = true;
            LightSemaphore_Release(&present_go_, 1);
            return true;
        }
        if (double_eyes_ && inbetween) {
            // To the GPU now, without its transfer: with no eye marked used
            // citro3d's frame end transfers nothing and asks for no swap.
            cur_eye_[0]->used = false;
            if (cur_eye_[1] != nullptr) cur_eye_[1]->used = false;
            // GX_CMDLIST_FLUSH: what the GPU reads that the CPU wrote is
            // flushed where it is written (vertices below in this function,
            // textures as they are decoded, the recorded frame's command
            // buffer), not the whole linear heap at every frame end, which
            // took milliseconds a frame on the console.
            u64 te0 = svcGetSystemTick();
            C3D_FrameEnd(GX_CMDLIST_FLUSH);
            acc_end_ += svcGetSystemTick() - te0;
            inb_pending_ = true;
            inb_stereo_ = eyes == 2;
            finish_frame_bookkeeping(inbetween, sfd);
            // Not the list's last: to the screen from here, once the frame
            // before it is there.
            if (!hold) flush_pending_inbetween();
            return false;
        }
        if (kind != Present::Normal) wait_present_slot();
        u64 te0 = svcGetSystemTick();
        C3D_FrameEnd(GX_CMDLIST_FLUSH);     // (see the in-between's above)
        acc_end_ += svcGetSystemTick() - te0;
        finish_frame_bookkeeping(inbetween, sfd);
        return false;
    }

    // The game's frame recorded before its in-betweens (send_dl, with the
    // builder): what replay_and_present left for hand_off_recorded.
    bool record_only_ = false;
    rt64_3ds::StereoFrame rec_sfd_{};
    int rec_eyes_ = 1;
    bool rec_fill_ = false;
    C3D_RenderTarget* rec_eye_[2] = {};
    // ... and, once the in-betweens are drawn (the last one waiting for its
    // transfer), its bookkeeping as replay_and_present would have done it
    // and the presenter's job: the last in-between's transfer, then this
    // frame. drew_inb: an in-between was drawn (its GPU time is the
    // presenter's to read).
    bool hand_off_recorded(bool drew_inb) {
        dinb_kind_ = drew_inb ? gpu_pending_kind_ : -1;
        dinb_eyes_ = gpu_pending_eyes_;
        dinb_fill_ = gpu_pending_fill_;
        gpu_pending_kind_ = 0;
        gpu_pending_eyes_ = rec_eyes_;
        gpu_pending_fill_ = rec_fill_;
        // Snapshots, captures and the read-back read the game's frame.
        last_eye_[0] = rec_eye_[0];
        last_eye_[1] = rec_eye_[1];
        finish_frame_bookkeeping(false, rec_sfd_);
        job_deferred_ = true;
        job_stereo_ = rec_eyes_ == 2;
        job_inb_ = inb_pending_;
        job_inb_stereo_ = inb_stereo_;
        inb_pending_ = false;
        LightEvent_Clear(&inb_free_);
        deferred_frames_++;
        present_busy_ = true;
        LightSemaphore_Release(&present_go_, 1);
        return true;
    }

    bool shown_inbetween_ = false;              // the frame presented last is an in-between (captures say so)
    void finish_frame_bookkeeping(bool inbetween, const rt64_3ds::StereoFrame& sfd) {
        shown_inbetween_ = inbetween;
        interp_.note_presented_framebuffer(frame_.color_image);
        // Depth is read from top_ (read_depth_zp): with double eyes that
        // holds the last game frame, an in-between draws elsewhere.
        if (!(double_eyes_ && inbetween)) {
            prev_depth_image_ = (frame_.color_width == (uint32_t)geo_.w) ? frame_.depth_image : 0;
            // The depth grid read at the next frame start is this frame's: keep
            // the world projection terms that turn it into distances.
            prev_cam_world_ = frame_.cam_is_world;
            // What the left eye's world shift was drawn with (the aim sampler
            // predicts displacements in that image).
            prev_sep_ = sfd.on ? sfd.sep : 0.0f;
            prev_conv_ = sfd.conv;
            prev_m22_ = frame_.cam[10];
            prev_m32_ = frame_.cam[14];
        }
        // An in-between frame's textures are its game frame's (one texture
        // frame per display list: the cache's ageing and re-hashing count
        // lists).
        if (!inbetween) {
            last_uploads_ = textures_.uploads_this_frame();
            textures_.end_frame();
        }
    }

    // ---- the presenter thread (frame interpolation): ends the game's frame
    // of an interpolated pair once the in-between is on the screen, so the
    // display list returns without waiting for a refresh. citro3d is used by
    // one thread at a time: the gfx thread hands over an open frame (and
    // g_gpu_lock with it) and waits for it to come back before its next
    // citro3d call (await_presenter).
    Thread presenter_ = nullptr;
    LightSemaphore present_go_, present_done_;
    bool present_busy_ = false;                 // gfx thread: a frame is with the presenter
    volatile bool presenter_stop_ = false;
    volatile uint32_t presenter_wait_us_ = 0;   // presenter: time waited for the screen (stats)
    static void presenter_main(void* arg) {
        C3dRenderContext* self = static_cast<C3dRenderContext*>(arg);
        for (;;) {
            LightSemaphore_Acquire(&self->present_go_, 1);
            if (self->presenter_stop_) break;
            if (self->job_inb_) self->present_pending_inbetween();
            if (self->job_deferred_) self->present_deferred();
            else {
                const u64 w0 = svcGetSystemTick();
                for (int i = 0; i < 400 && gspIsPresentPending(GSP_SCREEN_TOP); i++) svcSleepThread(100000);
                const u64 w1 = svcGetSystemTick();
                C3D_FrameEnd(GX_CMDLIST_FLUSH);
                self->presenter_wait_us_ += (uint32_t)((w1 - w0) * 1000000ull / SYSCLOCK_ARM11);
            }
            LightLock_Unlock(&g_gpu_lock);
            LightSemaphore_Release(&self->present_done_, 1);
        }
    }

    // Double eyes (presenter thread): the game's frame of a pair, recorded
    // while the in-between was still on the GPU (begin_deferred), goes to
    // the GPU in two frames of citro3d's. First its clears and command
    // lists, as soon as the in-between's work is done (C3D_FrameBegin waits
    // for it): the draws run while the in-between waits for its refresh;
    // no eye is marked used, so citro3d transfers nothing and asks for no
    // swap. Then, once the in-between is on the screen and the draws are
    // done, the eyes' display transfers into the back buffer it left and
    // the swap at their end. The GPU runs its queue in order: the clears
    // come after the in-between's transfers out of its own targets anyway.
    void present_deferred() {
        const u64 t0 = svcGetSystemTick();
        C3D_FrameBegin(0);
        // citro3d asks for the in-between's swap (and stops its GPU clock)
        // in the queue's completion callback, just after the wait above can
        // return: the back buffer moves on then.
        for (int i = 0; i < 400 && (const void*)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr) == inb_fb_; i++) svcSleepThread(25000);
        const u64 t1 = svcGetSystemTick();
        job_inb_gpu_ms_ = C3D_GetDrawingTime() + job_inb_draw_ms_;   // its transfer, and its draws before it
        job_inb_draw_ms_ = 0.0f;
        LightEvent_Signal(&inb_free_);         // the next in-between may fill its vertex buffer
        for (int i = 0; i < dop_count_; i++) {
            const DeferredOp& op = dops_[i];
            if (op.fill != nullptr) C3D_RenderTargetClear(op.fill, op.bits, op.color, 0);
            else if (op.copy_dst != nullptr) copy_depth(op.copy_src, op.copy_dst, op.copy_bytes);
            else GX_ProcessCommandList(op.list, op.words * 4, 0);
        }
        C3D_FrameEnd(GX_CMDLIST_FLUSH);       // the recording was flushed as it ended (end_deferred)
        const u64 w0 = svcGetSystemTick();
        for (int i = 0; i < 400 && gspIsPresentPending(GSP_SCREEN_TOP); i++) svcSleepThread(100000);
        const u64 w1 = svcGetSystemTick();
        C3D_FrameBegin(0);
        const u64 t2 = svcGetSystemTick();
        link_eyes(0);
        top_->used = true;
        if (job_stereo_ && top_r_ != nullptr) top_r_->used = true;
        job_draw_gpu_ms_ = C3D_GetDrawingTime();
        // The transfers read the eyes in VRAM: no data cache to flush.
        C3D_FrameEnd(GX_CMDLIST_FLUSH);
        presenter_wait_us_ += (uint32_t)((w1 - w0) * 1000000ull / SYSCLOCK_ARM11);
        job_wait_inb_ = t1 - t0;
        job_wait_draws_ = t2 - w1;
    }
    // An in-between drawn without its transfer (replay_and_present), to the
    // screen once the frame before it is there (the caller waits for
    // that): its eyes' display transfers into the back buffer and the swap
    // at their end. C3D_FrameBegin waits for its draws, which have been on
    // the GPU since it was recorded; returns their GPU time.
    float transfer_inbetween(bool stereo) {
        C3D_FrameBegin(0);
        const float draws_ms = C3D_GetDrawingTime();
        LightEvent_Signal(&inb_free_);         // its vertices are read
        inb_fb_ = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
        link_eyes(1);
        inb_l_->used = true;
        if (stereo && inb_r_ != nullptr) inb_r_->used = true;
        // The transfers read the eyes in VRAM: no data cache to flush.
        C3D_FrameEnd(GX_CMDLIST_FLUSH);
        return draws_ms;
    }
    // The list's last in-between (presenter thread); present_deferred then
    // does the game's frame.
    void present_pending_inbetween() {
        const u64 w0 = svcGetSystemTick();
        for (int i = 0; i < 400 && gspIsPresentPending(GSP_SCREEN_TOP); i++) svcSleepThread(100000);
        const u64 w1 = svcGetSystemTick();
        job_inb_draw_ms_ = transfer_inbetween(job_inb_stereo_);
        presenter_wait_us_ += (uint32_t)((w1 - w0) * 1000000ull / SYSCLOCK_ARM11);
        job_inb_ = false;
        held_presented_++;
    }
    // The gfx thread's: an in-between before the list's last, or the last
    // when the game's frame after it can't be recorded on its own.
    void flush_pending_inbetween() {
        if (!inb_pending_) return;
        wait_present_slot();
        const u64 te0 = svcGetSystemTick();
        // Its draws' GPU time joins its transfer's, read at the next frame begin.
        gpu_pending_extra_ms_ += transfer_inbetween(inb_stereo_);
        acc_end_ += svcGetSystemTick() - te0;
        inb_pending_ = false;
        held_flushed_++;
    }
    void start_presenter() {
        LightSemaphore_Init(&present_go_, 0, 1);
        LightSemaphore_Init(&present_done_, 0, 1);
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        // Above the gfx thread, on its core: it mostly sleeps.
        // (Azahar answers svcGetProcessorID with garbage: the default core then.)
        s32 core = svcGetProcessorID();
        if (core < 0 || core > 3) core = -2;
        presenter_ = threadCreate(presenter_main, this, 16 * 1024, prio > 0x19 ? prio - 1 : prio, core, true);
        if (presenter_ == nullptr && core != -2) presenter_ = threadCreate(presenter_main, this, 16 * 1024, prio > 0x19 ? prio - 1 : prio, -2, true);
        fprintf(stderr, "rt64-3ds: frame interpolation presenter %s (priority 0x%lx, core %ld)\n", presenter_ != nullptr ? "up" : "FAILED: frames end inline",
                (unsigned long)(prio > 0x19 ? prio - 1 : prio), (long)core);
    }
    void await_presenter() {
        if (!present_busy_) return;
        LightSemaphore_Acquire(&present_done_, 1);
        present_busy_ = false;
        if (job_deferred_) {
            // What the presenter read: the in-between's GPU time, and the
            // game frame's draws (its transfers are read at the next frame
            // begin, as ever).
            job_deferred_ = false;
            account_gpu(dinb_kind_, dinb_eyes_, dinb_fill_, job_inb_gpu_ms_);
            dinb_kind_ = -1;
            gpu_pending_extra_ms_ = job_draw_gpu_ms_;
            acc_presenter_gpu_wait_[0] += job_wait_inb_;
            acc_presenter_gpu_wait_[1] += job_wait_draws_;
        }
    }
    // The gfx thread's share of GPU time per frame kind (see the report).
    void account_gpu(int kind, int eyes, bool fill, float ms) {
        if (kind < 0) return;
        gpu_ms_[kind] += ms;
        gpu_frames_[kind]++;
        gpu_eyes_[kind] += (uint32_t)eyes;
        if (fill) fill_cleared_++;
    }
    void stop_presenter() {
        if (presenter_ == nullptr) return;
        await_presenter();
        presenter_stop_ = true;
        LightSemaphore_Release(&present_go_, 1);
        threadJoin(presenter_, 1000000000ull);
        presenter_ = nullptr;
    }

    // ---- the builder thread (double eyes, New 3DS): builds a list's first
    // in-between on core 1 while this thread records the game's frame (see
    // send_dl), work core 2 did in turn before. build_inbetween reads the
    // list's records and the interpreter's records of its triangles, which
    // nothing changes until the next list's run; the recording only reads
    // them too. BUILDER_OFF.TXT keeps it all on this thread.
    Thread builder_ = nullptr;
    LightSemaphore build_go_, build_done_;
    volatile bool builder_stop_ = false;
    bool build_busy_ = false;                   // gfx thread: the builder has inter_ (join_build)
    volatile bool build_ok_ = false;
    float build_weight_ = 0.0f;                 // the first in-between's weight, for the builder's match
    int build_skip_ = 0;                        // the builder's match found: 1 a camera cut, 2 nothing to move
    volatile u64 build_ticks_ = 0;              // the builder's time, this report
    static void builder_main(void* arg) {
        C3dRenderContext* self = static_cast<C3dRenderContext*>(arg);
        for (;;) {
            LightSemaphore_Acquire(&self->build_go_, 1);
            if (self->builder_stop_) break;
            __dmb();
            const u64 t0 = svcGetSystemTick();
            self->last_match_ = self->interp_.match_previous(self->build_weight_);
            const rt64_3ds::interp::MatchResult& m = self->last_match_;
            self->build_skip_ = m.camera_cut ? 1 : (m.xf_interp + m.pj_interp + m.tiles_interp == 0) ? 2 : 0;
            self->build_ok_ = self->build_skip_ == 0 && self->interp_.build_inbetween(self->frame_, self->inter_);
            // Written on this core: flushed from it.
            if (self->inter_.verts.size() != 0) flush_dcache(self->inter_.verts.data(), self->inter_.verts.size() * sizeof(GpuVertex));
            self->build_ticks_ = self->build_ticks_ + (svcGetSystemTick() - t0);
            __dmb();
            LightSemaphore_Release(&self->build_done_, 1);
        }
    }
    // ---- the builder's helper (New 3DS, core 3, which the application
    // otherwise leaves idle; the system's head tracking also runs there):
    // the second half of each in-between's
    // triangles (Interpreter::set_inbetween_helper), whichever thread builds
    // it. It writes a buffer of its own, which the building thread copies
    // into the vertex buffer, so only that thread writes what the GPU reads
    // and its flush covers it all. Heavy lists' builds took as long as their
    // read (DK64: 12-18 ms), and the gfx thread waited 5-10 ms for them.
    // BUILD_SPLIT_OFF.TXT: one thread.
    struct CoreHelper : rt64_3ds::Interpreter::Helper {
        Thread thread = nullptr;
        LightSemaphore go, done;
        volatile bool stop = false;
        void (*job)(void*) = nullptr;
        void* arg = nullptr;
        // This report's builds, the helper's time and the builder's wait for
        // it: written on core 3 and reset by the report, so a figure may now
        // and then be torn or lose a build (statistics only).
        volatile uint32_t jobs = 0;
        volatile u64 run_ticks = 0, wait_ticks = 0;
        static void main(void* p) {
            CoreHelper* self = static_cast<CoreHelper*>(p);
            for (;;) {
                LightSemaphore_Acquire(&self->go, 1);
                if (self->stop) break;
                __dmb();
                const u64 t0 = svcGetSystemTick();
                self->job(self->arg);
                self->run_ticks = self->run_ticks + (svcGetSystemTick() - t0);
                self->jobs = self->jobs + 1;
                __dmb();
                LightSemaphore_Release(&self->done, 1);
            }
        }
        void start(void (*j)(void*), void* a) override {
            job = j;
            arg = a;
            __dmb();
            LightSemaphore_Release(&go, 1);
        }
        void finish() override {
            const u64 t0 = svcGetSystemTick();
            LightSemaphore_Acquire(&done, 1);
            __dmb();
            wait_ticks = wait_ticks + (svcGetSystemTick() - t0);
        }
    };
    CoreHelper helper_;
    void start_helper() {
        if (FILE* f = rt64_3ds::data_fopen("BUILD_SPLIT_OFF.TXT", "r")) {
            fclose(f);
            fprintf(stderr, "rt64-3ds: in-betweens built by one thread (BUILD_SPLIT_OFF.TXT)\n");
            return;
        }
        LightSemaphore_Init(&helper_.go, 0, 1);
        LightSemaphore_Init(&helper_.done, 0, 1);
        // Below the system's threads on core 3 (head tracking's among them).
        helper_.thread = threadCreate(CoreHelper::main, &helper_, 32 * 1024, 0x2F, 3, true);
        if (helper_.thread != nullptr) interp_.set_inbetween_helper(&helper_);
        fprintf(stderr, "rt64-3ds: in-betweens' second half %s\n", helper_.thread != nullptr ? "on core 3" : "not on core 3 (refused): one thread builds them");
    }
    void stop_helper() {
        if (helper_.thread == nullptr) return;
        interp_.set_inbetween_helper(nullptr);
        helper_.stop = true;
        LightSemaphore_Release(&helper_.go, 1);
        threadJoin(helper_.thread, 1000000000ull);
        helper_.thread = nullptr;
    }
    void start_builder() {
        bool n3ds = false;
        if (R_FAILED(APT_CheckNew3DS(&n3ds)) || !n3ds) return;
        if (rt64_3ds::settings().no_core2) {
            fprintf(stderr, "rt64-3ds: in-between builder off (two cores)\n");
            return;
        }
        if (FILE* f = rt64_3ds::data_fopen("BUILDER_OFF.TXT", "r")) {
            fclose(f);
            fprintf(stderr, "rt64-3ds: in-between builder off (BUILDER_OFF.TXT)\n");
            return;
        }
        LightSemaphore_Init(&build_go_, 0, 1);
        LightSemaphore_Init(&build_done_, 0, 1);
        // Core 1, below the audio task's thread there (0x24).
        builder_ = threadCreate(builder_main, this, 32 * 1024, 0x2E, 1, true);
        fprintf(stderr, "rt64-3ds: in-between builder %s (core 1)\n", builder_ != nullptr ? "up" : "FAILED: in-betweens built on this thread");
        start_helper();
    }
    void kick_build() {
        build_busy_ = true;
        __dmb();
        LightSemaphore_Release(&build_go_, 1);
    }
    bool join_build() {
        if (!build_busy_) return true;
        LightSemaphore_Acquire(&build_done_, 1);
        __dmb();
        build_busy_ = false;
        return build_ok_;
    }
    void stop_builder() {
        if (builder_ != nullptr) {
            join_build();
            builder_stop_ = true;
            LightSemaphore_Release(&build_go_, 1);
            threadJoin(builder_, 1000000000ull);
            builder_ = nullptr;
        }
        stop_helper();
    }

    // Frame interpolation: wait until the frame presented before has been
    // put on the screen (its swap is processed at the next VBlank). Drawing
    // the next one into the back buffer before that would replace it.
    void wait_present_slot() {
        const u64 w0 = svcGetSystemTick();
        for (int i = 0; i < 200 && gspIsPresentPending(GSP_SCREEN_TOP); i++) svcSleepThread(250000);
        acc_vsync_ += svcGetSystemTick() - w0;
    }

    // ---- diagnostics (INTERP_CHECK.TXT): a flat draw (orthographic
    // triangles, or a rectangle) shown in another place or size for one
    // frame, as DK64's first-person scope and crosshair were seen in a small
    // box near the top-left (an in-between pairing its projection with
    // another scale's). Off by default: the checks cost the gfx thread
    // every frame.
    bool interp_check_ = false;
    // Draws are matched between frames by what they are (kind, vertex
    // count, projection tag, texture address, and which occurrence of
    // those), not by their place in the list: sprites coming and going (a
    // lens flare) shift the rest. Not by vertex address: DK64 builds its
    // vertices into a buffer of each frame's. Boxes, not vertices: clipping
    // may order a draw's vertices differently; and only draws near the
    // screen, as a box clipped at the guard band is no measure.
    struct FlatBox { float b[4]; uint64_t sig; uint8_t kind; bool persp, on; uint32_t proj_id; };
    static uint64_t flat_sig(const rt64_3ds::DrawRecord& d) {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
        mix(d.kind); mix(d.count); mix(d.proj_id); mix(d.tex[0].valid ? d.tex[0].addr : 0);
        return h;
    }
    // The flat draws of a frame (the in-between, its game frame).
    static void flat_boxes(const rt64_3ds::FrameRecord& f, std::vector<FlatBox>& out) {
        out.assign(f.draws.size(), FlatBox{ { 0, 0, 0, 0 }, 0, 0, false, false, 0 });
        std::unordered_map<uint64_t, uint32_t> seen;
        for (size_t i = 0; i < f.draws.size(); i++) {
            const rt64_3ds::DrawRecord& d = f.draws[i];
            if ((d.kind == rt64_3ds::DrawRecord::Tris && d.perspective) || d.count == 0 || d.first + d.count > f.verts.size()) continue;
            FlatBox& h = out[i];
            const uint64_t base = flat_sig(d);
            h.sig = base ^ ((uint64_t)(seen[base]++) * 0x9E3779B97F4A7C15ull);
            h.b[0] = h.b[1] = 1e9f; h.b[2] = h.b[3] = -1e9f;
            h.kind = (uint8_t)d.kind; h.persp = d.perspective; h.proj_id = d.proj_id;
            for (uint32_t k = d.first; k < d.first + d.count; k++) {
                const rt64_3ds::Vtx3ds& v = f.verts[k];
                if (!(v.w > 0.0f)) continue;
                const float x = v.x / v.w, y = v.y / v.w;
                h.b[0] = std::min(h.b[0], x); h.b[1] = std::min(h.b[1], y);
                h.b[2] = std::max(h.b[2], x); h.b[3] = std::max(h.b[3], y);
                h.on = true;
            }
            if (!(h.b[0] > -200.0f && h.b[1] > -200.0f && h.b[2] < 600.0f && h.b[3] < 450.0f)) h.on = false;
        }
    }
    static const FlatBox* find_flat(const std::vector<FlatBox>& v, uint64_t sig) {
        for (const FlatBox& h : v) if (h.on && h.sig == sig) return &h;
        return nullptr;
    }
    static void box_text(const FlatBox* h, char* out, size_t n) {
        if (h == nullptr) snprintf(out, n, "-");
        else snprintf(out, n, "%.0f,%.0f-%.0f,%.0f", h->b[0], h->b[1], h->b[2], h->b[3]);
    }
    std::vector<FlatBox> flat_[3];              // game frames: [2] the last, [1] the one before, [0] the one before that
    uint32_t flat_seq_[3] = {}, flat_frame_[3] = {};
    uint32_t list_seq_ = 0;                     // lists sent, ever
    uint32_t flat_logged_ = 0;                  // lines this session (at most 60)
    // A game frame's flat boxes (replay_and_present), then its check.
    void note_flat_boxes() {
        if (flat_logged_ >= 60) return;
        flat_[0].swap(flat_[1]); flat_[1].swap(flat_[2]);
        flat_seq_[0] = flat_seq_[1]; flat_seq_[1] = flat_seq_[2];
        flat_frame_[0] = flat_frame_[1]; flat_frame_[1] = flat_frame_[2];
        flat_boxes(frame_, flat_[2]);
        flat_seq_[2] = list_seq_;
        flat_frame_[2] = frames_;
        check_flat_blips();
    }
    // Game frames: the same box two game frames apart, another between.
    void check_flat_blips() {
        if (flat_frame_[0] == 0) return;
        const std::vector<FlatBox>& b = flat_[1];
        int blips = 0;
        const FlatBox* x = nullptr;
        const FlatBox* y = nullptr;
        for (const FlatBox& m : b) {
            if (!m.on) continue;
            const FlatBox* a = find_flat(flat_[0], m.sig);
            const FlatBox* c = find_flat(flat_[2], m.sig);
            if (a == nullptr || c == nullptr) continue;
            float same = 0.0f, off = 0.0f;
            for (int e = 0; e < 4; e++) {
                same = std::max(same, std::fabs(a->b[e] - c->b[e]));
                off = std::max(off, std::min(std::fabs(m.b[e] - a->b[e]), std::fabs(m.b[e] - c->b[e])));
            }
            if (same > 2.0f || off <= 8.0f) continue;
            if (blips++ == 0) { x = &m; y = c; }
        }
        if (blips == 0) return;
        flat_logged_++;
        char t0[64], t1[64];
        box_text(x, t0, sizeof(t0)); box_text(y, t1, sizeof(t1));
        fprintf(stderr, "rt64-3ds: game frame %u drew %d flat draw%s differently from the frames on either side (first: %s, projection id %lx): %s there, %s around it\n",
                flat_frame_[1], blips, blips == 1 ? "" : "s",
                x->kind == rt64_3ds::DrawRecord::Tris ? "triangles" : x->kind == rt64_3ds::DrawRecord::TexRect ? "texture rectangle" : "fill rectangle",
                (unsigned long)x->proj_id, t0, t1);
    }
    // In-betweens: each flat draw between where the game frames on either
    // side put it (6 pixels' slack on each edge of its box); without the
    // one before (the draw is new), no more than a quarter larger or
    // smaller than in its game frame (32 pixels or more across). Not the
    // world's or the sky's (a lens flare's sprites animate). In
    // replay_and_present, the in-between swapped into frame_ and its game
    // frame in inter_.
    void check_inbetween_flat() {
        if (flat_logged_ >= 60) return;
        // The game frame before this list's: the last one noted, unless that
        // is this list's own (recorded before its in-betweens).
        const std::vector<FlatBox>& before = flat_seq_[2] == list_seq_ ? flat_[1] : flat_[2];
        std::vector<FlatBox> game, inb;
        flat_boxes(inter_, game);
        flat_boxes(frame_, inb);
        for (size_t i = 0; i < inb.size() && i < game.size(); i++) {
            const FlatBox& n = inb[i];
            const FlatBox& g = game[i];
            if (!n.on || !g.on) continue;
            if (i < classes_.size() && (classes_[i] == (uint8_t)rt64_3ds::StereoClass::World || classes_[i] == (uint8_t)rt64_3ds::StereoClass::Infinity)) continue;
            const FlatBox* p = find_flat(before, g.sig);
            float out = 0.0f;
            if (p != nullptr) {
                for (int e = 0; e < 4; e++) {
                    const float lo = std::min(p->b[e], g.b[e]) - 6.0f, hi = std::max(p->b[e], g.b[e]) + 6.0f;
                    out = std::max(out, std::max(lo - n.b[e], n.b[e] - hi));
                }
            }
            else {
                const float gw = g.b[2] - g.b[0], gh = g.b[3] - g.b[1], nw = n.b[2] - n.b[0], nh = n.b[3] - n.b[1];
                if (gw >= 32.0f && gh >= 32.0f && (nw < gw * 0.75f || nw > gw * 1.25f || nh < gh * 0.75f || nh > gh * 1.25f)) out = std::max(std::fabs(nw - gw), std::fabs(nh - gh));
            }
            if (out <= 0.0f) continue;
            flat_logged_++;
            char t0[64], t1[64], t2[64];
            box_text(p, t0, sizeof(t0)); box_text(&n, t1, sizeof(t1)); box_text(&g, t2, sizeof(t2));
            const rt64_3ds::DrawRecord& d = frame_.draws[i];
            fprintf(stderr, "rt64-3ds: in-between put flat draw %zu of %zu %.0f px outside its game frames (frame %u, class %d, projection id %lx%s, %u verts): "
                            "before %s, in-between %s, game frame %s\n",
                    i, inb.size(), out, frames_, i < classes_.size() ? (classes_[i] == kReticle ? -1 : (int)classes_[i]) : -2,
                    (unsigned long)d.proj_id, d.mv_auto ? ", modelview untagged" : "", d.count, t0, t1, t2);
            char why[600];
            interp_.describe_inbetween_draw((uint32_t)i, why, sizeof(why));
            fprintf(stderr, "rt64-3ds:   %s\n", why);
            if (flat_logged_ >= 60) return;
        }
    }
    // The in-between frame and its textures take the place of the game's
    // frame for its replay (and back).
    void swap_inbetween() {
        frame_.swap(inter_);
        bound_.swap(inter_bound_);
    }

    // In-between frames for a game frame at `rate` (what it asks for): one
    // for each refresh of the screen's it lasts, but its own. 30 fps: one, at
    // weight 0.5; 20 (Banjo's cutscenes): two, at 1/3 and 2/3; 15: three.
    static int inbetweens_for(uint32_t rate) {
        if (rate < 15 || rate >= 60) return 0;
        const int refreshes = (int)((60 + rate / 2) / rate);
        return refreshes < 2 ? 1 : refreshes > 4 ? 3 : refreshes - 1;
    }

    // Frame interpolation: whether this list gets in-between frames; the
    // first is built into inter_ (the others as they are drawn: see the
    // present path). Sets engaged_ while the game asks for less than 60
    // (the frames are paced to the screen's refreshes then).
    bool make_inbetween(const OSTask* task, bool behind) {
        const uint32_t rate = interp_.stats().refresh_rate;
        const int wanted = inbetweens_for(rate);
        const bool rate_ok = wanted > 0;
        const bool related = inbetweens_for(prev_rate_) > 0 && prev_list_ok_;
        // What a second run of the list can't reproduce: a copy of the screen
        // taken before drawing (snapshots), images drawn off screen to be
        // sampled by later frames (render to texture), depth written back
        // for the game, a frame that ends off screen, a blanked video. The
        // copies of the frame drawn so far (sample_drawn_frame) are drawn
        // anew in every frame from that frame, so they can. Depth written
        // back is read from top_ before the game's frame of the next pair:
        // with one set of eyes that is the in-between's depth, with double
        // eyes (the in-betweens draw into their own) the last game frame's,
        // as without interpolation.
        const rt64_3ds::RenderDesc& desc = rt64_3ds::render_desc();
        uint32_t* unsafe = nullptr;
        if (frame_.snapshot_request) unsafe = &inb_snapshot_;
        else if (desc.depth_to_rdram && !double_eyes_) unsafe = &inb_depth_;
        else if (vi_blanked_) unsafe = &inb_blank_;
        if (unsafe == nullptr && desc.render_to_texture) for (const rt64_3ds::DrawRecord& d : frame_.draws) if (d.rtt >= 0) { unsafe = &inb_rtt_; break; }
        if (unsafe == nullptr) for (const rt64_3ds::RttImage& img : frame_.rtts) if (img.addr != 0 && img.addr == frame_.color_image) unsafe = &inb_offscreen_;
        const bool safe = unsafe == nullptr;
        const bool prev_60 = inbetweens_for(prev_rate_) == 0;
        prev_rate_ = rate;
        prev_list_ok_ = safe;
        engaged_ = rate_ok;
        if (!rate_ok) return false;
        if (!safe) { inb_unsafe_++; (*unsafe)++; return false; }
        if (!related) { inb_unrelated_++; (prev_60 ? inb_after60_ : inb_after_unsafe_)++; return false; }
        // The pair may take the game's whole frame time. Tried on a console
        // with a setting from 70 to 100% (Banjo, New 3DS, 2026-10-08): with
        // probes that ramp up (send_dl's behind), 100% kept 60 in 158 of 186
        // seconds, the file select included.
        const float budget = 1000.0f / (float)rate;
        // As many in-betweens as the budget affords (fewer show the game's
        // frame for the refreshes left). Over budget for a while: try them
        // anyway, for kProbeLists lists in a row, to measure them again. One
        // alone measures high: the gfx thread waits out the builder, which in
        // steady running overlaps the waits for the screen (Banjo's file
        // select: 33.8 ms alone, 23.8 running). A run that still doesn't fit
        // makes the next wait twice as long, up to kProbeEveryMax lists.
        int n = wanted;
        while (n > 1 && real_ema_ + (float)n * inb_ema_ > budget) n--;
        const bool fits = real_ema_ + (float)n * inb_ema_ <= budget;
        if (fits) { budget_drops_ = 0; probe_left_ = 0; probe_every_ = kProbeEvery; }
        else if (probe_left_ == 0 && budget_drops_ >= probe_every_) {
            probe_left_ = kProbeLists;
            budget_drops_ = 0;
            probe_every_ = std::min(probe_every_ * 2, kProbeEveryMax);
            probes_++;
        }
        const bool probing = !fits && probe_left_ > 0;
        if (probing) probe_left_--;
        const bool afford = !behind && (fits || probing);
        inb_count_ = n;
        // On the builder (core 1) while this thread records the game's frame
        // (send_dl), when that frame will be recorded on its own: the match
        // with the list before and the first in-between's build, both. Not
        // for a frame something reads back (captures, screenshots, the GPU
        // debug mode), which a frame of citro3d's records as ever. A camera
        // cut, or nothing to move, then shows at join_build (build_skip_):
        // the game's frame goes alone.
        if (afford && builder_ != nullptr && double_eyes_ && presenter_ != nullptr && !gpu_debug_ && !fb_capture_pending_ && !g_shot_request &&
            g_seq_request == 0 && !(seq_count_ > 0 && (int)frames_ + 1 >= seq_first_ && (int)frames_ < seq_first_ + seq_count_)) {
            wait_inbetween_vbo();
            inter_.clear();
            inter_.verts.set_storage((rt64_3ds::Vtx3ds*)vbo_[2], kVboVerts);
            build_weight_ = 1.0f / (float)(n + 1);
            kick_build();
            return true;
        }
        const u64 tm0 = svcGetSystemTick();
        last_match_ = interp_.match_previous(afford ? 1.0f / (float)(n + 1) : -1.0f);
        acc_match_ += svcGetSystemTick() - tm0;
        if (behind) { inb_late_++; if (behind_resync_) inb_late_resync_++; return false; }
        if (!afford) { inb_budget_++; budget_drops_++; return false; }
        if (last_match_.camera_cut) { inb_cut_++; return false; }
        if (last_match_.xf_interp + last_match_.pj_interp + last_match_.tiles_interp == 0) { inb_nothing_++; return false; }
        wait_inbetween_vbo();
        inter_.clear();
        inter_.verts.set_storage((rt64_3ds::Vtx3ds*)vbo_[2], kVboVerts);
        (void)task;
        if (!interp_.build_inbetween(frame_, inter_)) { inb_diverged_++; return false; }
        return true;
    }
    // Double eyes: the last in-between may still be on the GPU (the game's
    // frame after it was handed over without waiting for it), reading the
    // vertices the next one is built into.
    void wait_inbetween_vbo() {
        if (present_busy_ && job_deferred_) {
            const u64 tw = svcGetSystemTick();
            LightEvent_WaitTimeout(&inb_free_, 200 * 1000000ll);
            acc_vbo_wait_ += svcGetSystemTick() - tw;
        }
    }

    // Frame read-back (see read_back_frame): the previous frame's left eye
    // into the buffer not holding the last finished one. Out of the frame,
    // like the snapshot copy: citro3d waits for the frame's GPU work first,
    // and C3D_FrameBegin for the transfer.
    int readback_next_ = 0;
    bool start_readback() {
        if (!rt64_3ds::first_person_scene() || g_readback[0] == nullptr || frames_ < 2) {
            if (g_readback_ready >= 0) {
                LightLock_Lock(&g_readback_lock);
                g_readback_ready = -1;
                LightLock_Unlock(&g_readback_lock);
            }
            return false;
        }
        // Never the ready one, the only one a reader touches.
        const int next = g_readback_ready == 0 ? 1 : 0;
        const u32 kFlags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                           GX_TRANSFER_IN_FORMAT(color16_ ? GX_TRANSFER_FMT_RGB565 : GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB5A1) |
                           GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
        C3D_SyncDisplayTransfer((u32*)last_eye_[0]->frameBuf.colorBuf, GX_BUFFER_DIM(240, 400),
                                (u32*)g_readback[next], GX_BUFFER_DIM(240, 400), kFlags);
        readback_next_ = next;
        return true;
    }
    void finish_readback() {
        // Done now (C3D_FrameBegin waited); drop any stale cached lines.
        GSPGPU_InvalidateDataCache(g_readback[readback_next_], 240 * 400 * sizeof(u16));
        LightLock_Lock(&g_readback_lock);
        g_readback_ready = readback_next_;
        LightLock_Unlock(&g_readback_lock);
    }

    void copy_snapshot() {
        {
            // Outside a frame (C3D_SyncTextureCopy then waits for the last
            // frame's GPU work and starts the copy, and the next
            // C3D_FrameBegin waits for the copy to finish; inside one it queues
            // a frame split and the copy on citro3d's 32-entry GX queue, which
            // 50 copies overflow: a PANIC). The target still holds the last
            // presented frame: copy it, one 8x8-tile row at a time, into the
            // wider texture. Sizes are in
            // bytes; a tile row of the 240-wide buffer is 30 tiles.
            // TextureCopy line widths and gaps are in 16-byte units.
            const u32 bpp = color16_ ? 2 : 4;
            const u32 row = 240 * 8 * bpp, gap = (256 - 240) * 8 * bpp;
            // SNAPSHOT_CPU.TXT: copy with the CPU, to check the path in the
            // emulator (which keeps sampling a stale texture after a GPU
            // copy). Emulator only: the console faults on CPU writes to VRAM.
            static int cpu_copy = -1;
            if (cpu_copy < 0) {
                FILE* f = rt64_3ds::data_fopen("SNAPSHOT_CPU.TXT", "r");
                cpu_copy = f ? 1 : 0;
                if (f) fclose(f);
            }
            if (cpu_copy == 1) {
                const u8* src = (const u8*)last_eye_[0]->frameBuf.colorBuf;
                u8* dst = (u8*)snapshot_.data;
                for (u32 tr = 0; tr < 400 / 8; tr++) memcpy(dst + tr * (row + gap), src + tr * row, row);
            }
            else {
                // Flags bit 3 selects the texture-copy mode; libctru passes
                // the flags through, and without it the engine ran a display
                // transfer with whatever dimensions the last one left (our
                // RGB8 screen output): the scrambled snapshot.
                constexpr u32 kTextureCopyMode = BIT(3);
                C3D_SyncTextureCopy((u32*)last_eye_[0]->frameBuf.colorBuf, GX_BUFFER_DIM(row / 16, 0),
                                    (u32*)snapshot_.data, GX_BUFFER_DIM(row / 16, gap / 16), 240 * 400 * bpp,
                                    kTextureCopyMode);
            }
            snapshots_++;
            // Checked once the copy has finished: out of a frame citro3d
            // returns before it does on the console (gspWaitForPPF sees the
            // last frame's display transfer); C3D_FrameBegin waits for it.
            check_snapshot_ = snapshots_ <= 3;
        }
    }

    // Self-check (CPU reads of VRAM are fine): the copy must hold the colour
    // buffer tile row for tile row.
    void verify_snapshot() {
        const u32 bpp = color16_ ? 2 : 4;
        const u32 row = 240 * 8 * bpp, gap = (256 - 240) * 8 * bpp;
        const u32* src = (const u32*)last_eye_[0]->frameBuf.colorBuf;
        const u32* dst = (const u32*)snapshot_.data;
        u32 bad = 0, seed = 0x1234567u;
        for (int i = 0; i < 512; i++) {
            seed = seed * 1103515245u + 12345u;
            const u32 tr = (seed >> 8) % 50, off = (seed >> 3) % (row / 4);
            if (src[tr * (row / 4) + off] != dst[tr * ((row + gap) / 4) + off]) bad++;
        }
        fprintf(stderr, "rt64-3ds: snapshot copy %u: %u/512 words differ from the frame\n", snapshots_, bad);
    }

    // RenderDesc::fill_clear. Games clear their framebuffer with a fill
    // rectangle (Goemon: the first draw of every frame, then a sky that
    // covers it anyway); drawn, it is a full screen of fragments per eye on
    // top of the memory fill that clears the eye. When the frame's first
    // draw into the eye is a fill-mode rectangle that writes every pixel of
    // the eye exactly as the replay would draw it (whole target in the
    // vertices and the scissor, no blending, an alpha test it passes, no
    // stereo shift), the eyes are cleared to its colour and it is skipped:
    // the same pixels. Returns its index (and the clear value), or -1.
    bool fill_clear_ = false;
    uint32_t fill_cleared_ = 0;                  // frames that took it, this report
    int leading_fill(const rt64_3ds::StereoFrame& sf, u32* value) const {
        if (frame_.frame_sample_last >= 0) return -1;     // frames copying themselves draw in runs: as they are
        for (size_t i = 0; i < frame_.draws.size(); i++) {
            const rt64_3ds::DrawRecord& d = frame_.draws[i];
            if (d.rtt >= 0) continue;                     // an off-screen image's: another target
            if ((int)i < draw_lo_ || (int)i > draw_hi_) return -1;
            if (d.kind != rt64_3ds::DrawRecord::FillRect || ((d.othermode_h >> 20) & 3) != 3 || d.count != 6 || draw_blends(d)) return -1;
            if (d.first + d.count > frame_.verts.size()) return -1;
            // The alpha test apply_state sets, against the fill alpha.
            const uint32_t l = d.othermode_l, ac = l & 3;
            const uint8_t a = d.prim[3];
            if (ac == 1 && a < (d.blend[3] ? d.blend[3] : 1)) return -1;
            if (ac == 3 && a == 0) return -1;
            if (ac != 1 && ac != 3 && (l & 0x3000) == 0x3000 && a < 0x80) return -1;
            // The scissor apply_state sets covers the target.
            float x0 = geo_.screen_x((float)d.scissor[0]), x1 = geo_.screen_x((float)d.scissor[2]);
            const float y0 = geo_.screen_y((float)d.scissor[1]), y1 = geo_.screen_y((float)d.scissor[3]);
            if (d.scissor[0] <= 0 && d.scissor[2] >= geo_.w) { x0 = 0.0f; x1 = 400.0f; }
            if (lroundf(x0) > 0 || lroundf(x1) < 400 || lroundf(y0) > 0 || lroundf(y1) < 240) return -1;
            // So do its vertices, unshifted in either eye.
            float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
            for (uint32_t k = d.first; k < d.first + d.count; k++) {
                const rt64_3ds::Vtx3ds& v = frame_.verts[k];
                if (v.w <= 0.0f) return -1;
                minx = std::min(minx, v.x / v.w); maxx = std::max(maxx, v.x / v.w);
                miny = std::min(miny, v.y / v.w); maxy = std::max(maxy, v.y / v.w);
            }
            if (minx > -geo_.x0 + 0.5f || maxx < (float)geo_.w + geo_.x0 - 0.5f || miny > 0.5f || maxy < (float)geo_.h - 0.5f) return -1;
            if (sf.on) {
                if (i >= classes_.size() || classes_[i] == kReticle) return -1;
                for (int eye = 1; eye >= -1; eye -= 2) {
                    float sa = 0.0f, sb = 0.0f;
                    rt64_3ds::stereo_shift(sf, (rt64_3ds::StereoClass)classes_[i], d, eye, &sa, &sb);
                    if (sa != 0.0f || sb != 0.0f) return -1;
                }
            }
            // The TEV's output is the fill colour (prim) as is; the target
            // keeps it in its own format.
            const u32 r = d.prim[0], g = d.prim[1], b = d.prim[2];
            *value = color16_ ? (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)) : ((r << 24) | (g << 16) | (b << 8) | a);
            return (int)i;
        }
        return -1;
    }

    // Ghost reduction, the desktop's GhostReduce (StereoComposePS.hlsl):
    // (c - 0.5) * contrast + 0.5, then v * (1 - floor) + floor, on each eye's
    // finished image. That is one affine map, c * A + B, drawn as a quad of
    // colour B blended over the eye with the constant blend colour A. Frame
    // copies taken from the eye (the pause background, the zipper) get it
    // again when drawn, a second, slighter squeeze.
    float ghost_a_ = 1.0f;
    bool write_ghost_quad(GpuVertex* q) {
        const rt64_3ds::Settings& st = rt64_3ds::settings();
        const float c = (float)st.ghost_contrast / 100.0f, f = (float)st.ghost_black_floor / 100.0f;
        if (c >= 1.0f && f <= 0.0f) return false;
        ghost_a_ = c * (1.0f - f);
        const float b = (0.5f - 0.5f * c) * (1.0f - f) + f;
        const uint8_t bb = (uint8_t)(b * 255.0f + 0.5f);
        const float l = -geo_.x0, r = (float)geo_.w + geo_.x0, bot = (float)geo_.h;
        const float xs[6] = { l, r, r, l, r, l }, ys[6] = { 0, 0, bot, 0, bot, bot };
        for (int i = 0; i < 6; i++) {
            q[i] = GpuVertex{ xs[i], ys[i], 0.0f, 1.0f, 0.0f, 0.0f, bb, bb, bb, 255 };
        }
        return true;
    }
    void ghost_pass(uint32_t first) {
        for (int i = 0; i < 6; i++) {
            C3D_TexEnv* env = C3D_GetTexEnv(i);
            C3D_TexEnvInit(env);
            if (i == 0) C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            else C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        }
        const u32 a = (u32)(ghost_a_ * 255.0f + 0.5f);
        C3D_BlendingColor(a | (a << 8) | (a << 16) | 0xFF000000u);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_CONSTANT_COLOR, GPU_ZERO, GPU_ONE);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, u_stereo_, 0.0f, 0.0f, 0.0f, 0.0f);
        C3D_DrawArrays(GPU_TRIANGLES, first, 6);
        // Everything the draws cache is stale now.
        last_sa_ = last_sb_ = 1e30f;
        last_cc_[0] = last_cc_[1] = 0xFFFFFFFFu;
        misc_valid_ = false;
        last_plan_.stages = -1;
    }

    // Blending: force-blend with memory colour by source alpha.
    static bool draw_blends(const rt64_3ds::DrawRecord& d) {
        const uint32_t l = d.othermode_l, cyc = (d.othermode_h >> 20) & 3;
        const bool force_bl = (l & 0x4000) != 0;
        const uint32_t m2 = (cyc == 1) ? ((l >> 20) & 3) : ((l >> 22) & 3);   // second cycle in 2-cycle mode
        const uint32_t b2 = (cyc == 1) ? ((l >> 16) & 3) : ((l >> 18) & 3);
        return force_bl && m2 == 1 && (b2 == 0 || b2 == 1);
    }

    // fallbacks: counts the draws whose combiner is inexact (null: don't).
    void apply_state(const rt64_3ds::DrawRecord& d, size_t index, int* fallbacks) {
        u64 tp0 = tick();
        // Textures.
        float us = 1.0f, vs = 1.0f, us1 = 1.0f, vs1 = 1.0f;
        for (int t = 0; t < 2; t++) {
            bool bound = false;
            if (d.tex[t].valid && d.tex[t].snapshot && snapshot_.data != nullptr) {
                if (last_tex_[t] != &snapshot_) {
                    C3D_TexBind(t, &snapshot_);
                    last_tex_[t] = &snapshot_; last_tex_param_[t] = 0xFFFFFFFFu;
                }
                if (t == 0) { us = 1.0f / 256.0f; vs = 1.0f / 512.0f; }
                else { us1 = 1.0f / 256.0f; vs1 = 1.0f / 512.0f; }
                bound = true;
            }
            else if (d.tex[t].valid && d.tex[t].frame) {
                // The frame drawn so far (RenderDesc::sample_drawn_frame), in
                // the snapshot's layout.
                if (frame_rt_ != nullptr) {
                    if (last_tex_[t] != &frame_tex_) {
                        C3D_TexBind(t, &frame_tex_);
                        last_tex_[t] = &frame_tex_; last_tex_param_[t] = 0xFFFFFFFDu;
                    }
                    if (t == 0) { us = 1.0f / 256.0f; vs = 1.0f / 512.0f; }
                    else { us1 = 1.0f / 256.0f; vs1 = 1.0f / 512.0f; }
                    bound = true;
                }
            }
            else if (d.tex[t].valid && d.tex[t].rtt >= 0) {
                // An off-screen image drawn on the GPU (this frame or earlier).
                RttTarget& rt = rtt_[d.tex[t].rtt];
                if (rt.rt != nullptr) {
                    if (&rt.tex != last_tex_[t]) {
                        C3D_TexBind(t, &rt.tex);
                        last_tex_[t] = &rt.tex; last_tex_param_[t] = 0xFFFFFFFEu;
                    }
                    if (t == 0) { us = 1.0f / (float)rt.w; vs = 1.0f / (float)rt.h; }
                    else { us1 = 1.0f / (float)rt.w; vs1 = 1.0f / (float)rt.h; }
                    bound = true;
                }
            }
            else if (d.tex[t].valid) {
                // Looked up once for the frame (prefetch_textures), not per eye.
                const rt64_3ds::BoundTex b = index * 2 + t < bound_.size() ? bound_[index * 2 + t] : rt64_3ds::BoundTex{};
                if (b.tex != nullptr) {
                    GPU_TEXTURE_FILTER_PARAM f = d.tex[t].bilerp ? GPU_LINEAR : GPU_NEAREST;
                    GPU_TEXTURE_WRAP_PARAM ws = wrap_mode(d.tex[t].cms, d.tex[t].masks, d.tex[t].width);
                    GPU_TEXTURE_WRAP_PARAM wt = wrap_mode(d.tex[t].cmt, d.tex[t].maskt, d.tex[t].height);
                    uint32_t param = (uint32_t)f | ((uint32_t)ws << 4) | ((uint32_t)wt << 8);
                    if (b.tex != last_tex_[t] || param != last_tex_param_[t] || b.fresh) {
                        C3D_TexSetFilter(b.tex, f, f);
                        if (b.tex->maxLevel > 0) C3D_TexSetFilterMipmap(b.tex, f);   // between levels as within them
                        C3D_TexSetWrap(b.tex, ws, wt);
                        C3D_TexBind(t, b.tex);
                        last_tex_[t] = b.tex; last_tex_param_[t] = param;
                    }
                    if (t == 0) { us = b.uscale; vs = b.vscale; }
                    else { us1 = b.uscale; vs1 = b.vscale; }
                    bound = true;
                }
            }
            if (!bound && last_tex_[t] != &white_) {
                C3D_TexBind(t, &white_);
                last_tex_[t] = &white_; last_tex_param_[t] = 0xFFFFFFFFu;
            }
        }
        if (us != last_us_ || vs != last_vs_) {
            C3D_FVUnifSet(GPU_VERTEX_SHADER, u_uvscale_, us, vs, 0.0f, 0.0f);
            last_us_ = us; last_vs_ = vs;
        }
        {
            // TEXEL1: its own tile's shift and origin, its own texture size.
            float uv1[4];
            if (d.tex[1].valid && !d.tex[1].snapshot && !d.tex[1].frame) { uv1[0] = d.uv1[0] * us1; uv1[1] = d.uv1[1] * vs1; uv1[2] = d.uv1[2] * us1; uv1[3] = d.uv1[3] * vs1; }
            else { uv1[0] = us; uv1[1] = vs; uv1[2] = 0.0f; uv1[3] = 0.0f; }
            if (memcmp(uv1, last_uv1_, sizeof(uv1)) != 0) {
                C3D_FVUnifSet(GPU_VERTEX_SHADER, u_uv1_, uv1[0], uv1[1], uv1[2], uv1[3]);
                memcpy(last_uv1_, uv1, sizeof(uv1));
            }
        }
        u64 tp1 = tick();
        prof_[0] += tp1 - tp0;

        // Combiner: only re-sent when the plan changes (every C3D_TexEnv call
        // dirties the whole stage set).
        if (d.cc_w0 != last_cc_[0] || d.cc_w1 != last_cc_[1] || d.othermode_h != last_omh_ || d.othermode_l != last_oml_ ||
            memcmp(d.prim, last_prim_, 4) != 0 || memcmp(d.env, last_env_, 4) != 0 || memcmp(d.fog, last_fog_, 4) != 0 ||
            d.prim_lod_frac != last_plf_ || d.kind != last_kind_ || d.geometry_mode != last_gm_) {
            // Plans are cached: the same combiner/mode/colour set recurs across
            // draws and frames.
            // Exact key: every input plan_tev reads.
            PlanKey pk{};
            pk.w[0] = d.cc_w0; pk.w[1] = d.cc_w1; pk.w[2] = d.othermode_h; pk.w[3] = d.othermode_l;
            memcpy(&pk.w[4], d.prim, 4); memcpy(&pk.w[5], d.env, 4); memcpy(&pk.w[6], d.fog, 4);
            pk.w[7] = (uint32_t)d.prim_lod_frac | ((uint32_t)d.kind << 8) | (d.geometry_mode & 0x10000);
            uint32_t h = 2166136261u;
            for (uint32_t v : pk.w) { h ^= v; h *= 16777619u; h ^= h >> 15; }
            uint32_t slot = h & (kPlanCache - 1);
            const rt64_3ds::TevPlan* plan;
            if (plan_valid_[slot] && memcmp(&plan_keys_[slot], &pk, sizeof(pk)) == 0) {
                plan = &plan_cache_[slot];
            }
            else {
                rt64_3ds::plan_tev(d, plan_cache_[slot]);
                plan_keys_[slot] = pk; plan_valid_[slot] = true;
                plan = &plan_cache_[slot];
            }
            plan_fallbacks_ = plan->fallbacks;
            if (plan->stages != last_plan_.stages || plan->buffer_color != last_plan_.buffer_color ||
                memcmp(plan->stage, last_plan_.stage, sizeof(rt64_3ds::TevStage) * plan->stages) != 0) {
                rt64_3ds::apply_tev(*plan);
                last_plan_ = *plan;
            }
            last_cc_[0] = d.cc_w0; last_cc_[1] = d.cc_w1; last_omh_ = d.othermode_h; last_oml_ = d.othermode_l;
            memcpy(last_prim_, d.prim, 4); memcpy(last_env_, d.env, 4); memcpy(last_fog_, d.fog, 4);
            last_plf_ = d.prim_lod_frac; last_kind_ = d.kind; last_gm_ = d.geometry_mode;
        }
        if (fallbacks != nullptr && plan_fallbacks_ != 0) (*fallbacks)++;
        u64 tp2 = tick();
        prof_[1] += tp2 - tp1;

        // Depth: reversed map, so "less" becomes GEQUAL. Rects ignore depth
        // unless the RDP compares (they carry z = 0 = near).
        uint32_t l = d.othermode_l;
        // Everything below reads exactly these fields (and the target, whose
        // change invalidates the key): compared whole, never folded into a
        // lossy hash (a packed key let a 1-cycle rectangle equal a 2-cycle
        // triangle and scissor bits spill into othermode_l's).
        const MiscKey misc_key{ l, d.othermode_h & 0x300000u, { d.scissor[0], d.scissor[1], d.scissor[2], d.scissor[3] },
                                (uint8_t)d.kind, d.blend[3], (uint8_t)(d.rtt >= 0 ? 1 : 0) };
        if (misc_valid_ && misc_key == last_misc_) { prof_[2] += tick() - tp2; return; }
        last_misc_ = misc_key;
        misc_valid_ = true;
        bool zcmp = (l & 0x10) != 0 && d.kind == rt64_3ds::DrawRecord::Tris;
        bool zupd = (l & 0x20) != 0 && d.kind == rt64_3ds::DrawRecord::Tris;
        uint32_t zmode = (l >> 10) & 3;
        GPU_WRITEMASK wmask = zupd ? GPU_WRITE_ALL : GPU_WRITE_COLOR;
        // The frame copy keeps the alpha it was cleared to: full coverage,
        // what the RDP leaves in a 5551 framebuffer under opaque pixels and
        // what copy-mode alpha compares test (the eye's alpha is whatever
        // the combiner put out).
        if (frame_viewport_) wmask = (GPU_WRITEMASK)(wmask & ~GPU_WRITE_ALPHA);
        C3D_DepthTest(zcmp, zcmp ? GPU_GEQUAL : GPU_ALWAYS, wmask);
        // Decals sit on coplanar surfaces: nudge them toward the viewer
        // (reversed depth: nearer is larger).
        C3D_DepthMap(true, -1.0f, zmode == 3 ? 0.0004f : 0.0f);

        // Blending: force-blend with memory colour by source alpha.
        const bool blend = draw_blends(d);
        if (blend) C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
        else C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

        // Alpha compare, and the coverage-from-alpha "texture edge" modes.
        uint32_t ac = l & 3;
        if (ac == 1) C3D_AlphaTest(true, GPU_GEQUAL, d.blend[3] ? d.blend[3] : 1);
        else if (ac == 3) C3D_AlphaTest(true, GPU_GREATER, 0);
        else if ((l & 0x3000) == 0x3000) C3D_AlphaTest(true, GPU_GEQUAL, 0x80);
        else C3D_AlphaTest(false, GPU_ALWAYS, 0);

        // Culling.
        // Face culling is done in the interpreter (screen-space winding).
        C3D_CullFace(GPU_CULL_NONE);

        // Scissor in N64 pixels -> target pixels (rotated framebuffer).
        // NO_SCISSOR.TXT disables it (to see draws a wrong clip would hide).
        static int no_scissor = -1;
        if (no_scissor < 0) { FILE* f = rt64_3ds::data_fopen("NO_SCISSOR.TXT", "r"); no_scissor = f ? 1 : 0; if (f) fclose(f); }
        if (d.rtt >= 0) {
            // An off-screen image: the texture is its own clip.
            C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
            prof_[2] += tick() - tp2;
            return;
        }
        int x0 = (int)lroundf(geo_.screen_x((float)d.scissor[0])), y0 = (int)lroundf(geo_.screen_y((float)d.scissor[1]));
        int x1 = (int)lroundf(geo_.screen_x((float)d.scissor[2])), y1 = (int)lroundf(geo_.screen_y((float)d.scissor[3]));
        if (no_scissor) { x0 = 0; y0 = 0; x1 = 400; y1 = 240; }
        // The original game's own full-width scissor is stretched to the wide
        // screen: the patched projection already fills it.
        if (d.scissor[0] <= 0 && d.scissor[2] >= geo_.w) { x0 = 0; x1 = 400; }
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > 400) x1 = 400; if (y1 > 240) y1 = 240;
        // Mtx_OrthoTilt maps screen y = 0 to framebuffer x = 240 and the
        // screen's left edge to framebuffer y = 400: both axes run backwards.
        // The frame copy holds the eye kFrameRowOffset rows up.
        const u32 fy = frame_viewport_ ? kFrameRowOffset : 0;
        if (x1 <= x0 || y1 <= y0) C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, 0, 0);
        else C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - y1), (u32)(400 - x1) + fy, (u32)(240 - y0), (u32)(400 - x0) + fy);
        prof_[2] += tick() - tp2;
    }

public:
    // GFX_CAPTURE.TXT holds a frame number: that frame's RDRAM and OSTask go
    // to gfx_task.bin for tools/dl_test (host-side interpreter profiling).
    void maybe_capture(const OSTask* task) {
        int& want = capture_want_;
        // "pair <frame>": that frame's list and the next one (two lists of
        // one scene, for testing frame interpolation on the host).
        bool pair_second = false;
        if (capture_pair_next_) { capture_pair_next_ = false; want = (int)frames_; pair_second = true; }
        if (g_pair_request) {
            g_pair_request = false;
            if (want == -2) want = -1;
            if (want < 0) { want = (int)frames_; capture_pair_ = true; }
        }
        if (g_capture_request) {       // SELECT held on the console: this frame
            g_capture_request = false;
            if (want == -2) want = -1;
            if (want < 0) want = (int)frames_;
        }
        if (want == -2) {
            want = -1;
            if (FILE* f = rt64_3ds::data_fopen("GFX_CAPTURE.TXT", "r")) {
                char word[16] = {};
                if (fscanf(f, "%15s", word) == 1) {
                    if (strcmp(word, "snap") == 0) capture_snapshot_frames_ = 3;   // the first three frames drawing from a snapshot
                    else if (strcmp(word, "rtt") == 0) rtt_capture_armed_ = true;  // the first frame (after boot) drawing off screen
                    else if (strcmp(word, "seqrtt") == 0) {
                        // "seqrtt <count>": the rendered frames from the first one (after boot) drawing off screen
                        if (fscanf(f, "%d", &seq_rtt_count_) != 1) seq_rtt_count_ = 30;
                    }
                    else if (strcmp(word, "cam") == 0) {
                        // "cam <first frame> <count>": log the camera projection per frame
                        if (fscanf(f, "%d %d", &cam_log_first_, &cam_log_count_) != 2) cam_log_count_ = 0;
                    }
                    else if (strcmp(word, "store") == 0) {
                        // "store <delay>": the frame <delay> frames after the game first stores the screen
                        if (fscanf(f, "%d", &store_delay_) != 1) store_delay_ = 5;
                    }
                    else if (strcmp(word, "pair") == 0) {
                        if (fscanf(f, "%d", &want) != 1) want = -1;
                        capture_pair_ = want >= 0;
                    }
                    else if (strcmp(word, "seq") == 0) {
                        // "seq <first frame> <count>": the rendered frames only, one file each
                        if (fscanf(f, "%d %d", &seq_first_, &seq_count_) != 2) seq_count_ = 0;
                    }
                    else want = atoi(word);
                }
                fclose(f);
            }
        }
        if (cam_log_count_ > 0 && (int)frames_ >= cam_log_first_ && (int)frames_ < cam_log_first_ + cam_log_count_) cam_log_now_ = true;
        if (rtt_capture_armed_ && frames_ > 10) {
            // Keep this frame's RDRAM and task; send_dl keeps them if the
            // frame turns out to draw into an off-screen image.
            if (capture_copy_ == nullptr) capture_copy_ = (uint8_t*)malloc(16u * 1024 * 1024);
            if (capture_copy_ != nullptr) { memcpy(capture_copy_, rdram_, 16u * 1024 * 1024); rtt_capture_task_ = *task; }
        }
        if (want < 0 || (int)frames_ < want) return;   // (frames_ moves by two per list with in-between frames)
        want = -1;
        const bool pair_first = capture_pair_;
        capture_pair_ = false;
        // One capture at a time (the second of a pair may queue behind the
        // first): anything else is refused rather than kept waiting.
        const bool copy_held = capture_copy_ != nullptr && !rtt_capture_armed_;   // taken, not yet handed to the writer
        const int busy = capture::pending() + (copy_held ? 1 : 0);
        if (busy >= (pair_second ? capture::kQueue : 1)) {
            fprintf(stderr, "rt64-3ds: capture of frame %u refused: %s is still being written (%lu of %lu bytes)\n", frames_,
                    capture::g_writing[0] ? capture::g_writing : "the last one", (unsigned long)capture::g_done_bytes, (unsigned long)capture::g_total_bytes);
            capture::set_message(4, "Capture refused: busy (%s)", capture::g_writing[0] ? capture::g_writing : "writing");
            return;
        }
        if (pair_first) capture_pair_next_ = true;
        // Snapshot RDRAM and the task now; the slow SD write happens after
        // this frame is drawn (the game thread keeps changing RDRAM).
        if (capture_copy_ == nullptr) capture_copy_ = (uint8_t*)malloc(16u * 1024 * 1024);
        if (capture_copy_ == nullptr) {
            fprintf(stderr, "rt64-3ds: capture of frame %u refused: no memory for a 16 MB copy of RDRAM\n", frames_);
            capture::set_message(4, "Capture refused: no memory");
            capture_pair_next_ = false;
            return;
        }
        memcpy(capture_copy_, rdram_, 16u * 1024 * 1024);
        capture_task_ = *task;
        fprintf(stderr, "rt64-3ds: captured gfx task of frame %u\n", frames_);
        fb_capture_pending_ = true;   // the next frame begin saves what this task rendered
    }

    // After C3D_FrameBegin the eye target still holds the previous frame
    // (last_eye_: with double eyes the in-betweens' set when that was one):
    // untile it into linear memory and save it as gfx_frame.ppm (400x240),
    // to compare with the host reference render of gfx_task.bin.
    void maybe_capture_framebuffer() {
        if (!fb_capture_pending_) return;
        fb_capture_pending_ = false;
        // The finished frame, untiled from VRAM (the CPU can read it; the
        // emulator flushes its cached copy on such reads): 8x8 tiles, 30 per
        // tile row, Morton order inside a tile. Rotated (240 wide, 400 tall):
        // the host rotates it to the 400x240 screen.
        static const char kHeader[] = "P6\n240 400\n255\n";
        const size_t frame_bytes = sizeof(kHeader) - 1 + 240 * 400 * 3;
        uint8_t* frame = (uint8_t*)malloc(frame_bytes);
        if (frame != nullptr) {
            memcpy(frame, kHeader, sizeof(kHeader) - 1);
            uint8_t* o = frame + sizeof(kHeader) - 1;
            for (int r = 0; r < 400; r++) {
                for (int c = 0; c < 240; c++) {
                    uint32_t tile = (uint32_t)(r >> 3) * 30 + (uint32_t)(c >> 3);
                    uint32_t m = (c & 1) | ((r & 1) << 1) | ((c & 2) << 1) | ((r & 2) << 2) | ((c & 4) << 2) | ((r & 4) << 3);
                    eye_texel(last_eye_[0], tile * 64 + m, o);
                    o += 3;
                }
            }
        }
        if (g_readback_ready >= 0) {
            // In first person: the read-back buffer as a picture too, to check
            // its layout against the frame (gfx_readback.ppm, 400x240).
            if (FILE* f = rt64_3ds::data_fopen("gfx_readback.ppm", "wb")) {
                fprintf(f, "P6\n400 240\n255\n");
                const u16* src = g_readback[g_readback_ready];
                for (int sy = 0; sy < 240; sy++) {
                    for (int sx = 0; sx < 400; sx++) {
                        const u16 p = src[sx * 240 + 239 - sy];
                        const uint8_t rgb[3] = { (uint8_t)(((p >> 11) & 31) * 255 / 31), (uint8_t)(((p >> 6) & 31) * 255 / 31), (uint8_t)(((p >> 1) & 31) * 255 / 31) };
                        fwrite(rgb, 1, 3, f);
                    }
                }
                fclose(f);
            }
        }
        static int frame_captures = 0;
        CaptureJob* job = new CaptureJob{};
        if (frame_captures == 0) snprintf(job->frame_name, sizeof(job->frame_name), "gfx_frame.ppm");
        else snprintf(job->frame_name, sizeof(job->frame_name), "gfx_frame%d.ppm", frame_captures);
        // The task too, so several captures in one session are all kept.
        if (frame_captures == 0) snprintf(job->task_name, sizeof(job->task_name), "gfx_task.bin");
        else snprintf(job->task_name, sizeof(job->task_name), "gfx_task%d.bin", frame_captures);
        job->frame = frame;
        job->frame_bytes = frame_bytes;
        job->rdram = capture_copy_;
        job->task = capture_task_;
        capture_copy_ = nullptr;
        if (job->rdram != nullptr) {
            // A full capture: to the writer thread, the game plays on. (The
            // job is the writer's once queued: told first, named from here.)
            char task_name[sizeof(job->task_name)];
            memcpy(task_name, job->task_name, sizeof(task_name));
            capture::set_message(1, "Saving %s...", task_name);
            if (!capture::queue(job)) {
                fprintf(stderr, "rt64-3ds: capture of frame %u dropped: the writer is busy\n", frames_ - 1);
                capture::set_message(4, "Capture refused: busy (%s)", capture::g_writing);
                capture::free_job(job);
                return;
            }
            frame_captures++;
            log_kernel_objects("with the capture queued");
        }
        else {
            // A frame image only (sequence captures), written here.
            frame_captures++;
            capture::write_job(job);
            capture::free_job(job);
        }
        fprintf(stderr, "rt64-3ds: captured gfx frame %u (%s)\n", frames_ - 1, shown_inbetween_ ? "in-between" : "game frame");
    }

    // After C3D_FrameBegin the eye targets still hold the finished frame and
    // s3d_on_ still says whether it was drawn in 3D: the screenshot is made
    // here, from VRAM (the CPU may read it) and the touch screen's
    // framebuffer, and written by a background thread.
    void maybe_screenshot() {
        if (!g_shot_request) return;
        g_shot_request = false;
        char dir[224];
        rt64_3ds::data_path(dir, sizeof(dir), "screenshots");
        mkdir(dir, 0777);
        // Named by date and time; a second one within the same second gets
        // a number.
        static time_t last_time = 0;
        static int same_second = 0;
        const time_t now = time(nullptr);
        same_second = now == last_time ? same_second + 1 : 0;
        last_time = now;
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm_now);
        if (same_second > 0) snprintf(g_shot_name, sizeof(g_shot_name), "%s_%d", stamp, same_second + 1);
        else snprintf(g_shot_name, sizeof(g_shot_name), "%s", stamp);

        ShotJob* job = new ShotJob{};
        // An eye target: RGBA8 in 8x8 tiles, 30 per tile row, Morton order
        // inside; memory row r is screen x, column c is screen y from the
        // bottom (the tilted 240x400 layout).
        auto eye = [this](C3D_RenderTarget* t) {
            return [this, t](int x, int y, uint8_t* rgb) {
                const uint32_t r = (uint32_t)x, c = 239u - (uint32_t)y;
                const uint32_t tile = (r >> 3) * 30 + (c >> 3);
                const uint32_t m = (c & 1) | ((r & 1) << 1) | ((c & 2) << 1) | ((r & 2) << 2) | ((c & 4) << 2) | ((r & 4) << 3);
                eye_texel(t, tile * 64 + m, rgb);
            };
        };
        const char* suffix[3] = { "_top.bmp", "_top_right.bmp", "_bottom.bmp" };
        job->file[0] = make_bmp(400, 240, &job->bytes[0], eye(last_eye_[0]));
        if (s3d_on_ && last_eye_[1] != nullptr) job->file[1] = make_bmp(400, 240, &job->bytes[1], eye(last_eye_[1]));
        {
            // The touch screen: libctru's framebuffer, linear, tilted like
            // the top screen, RGB565 under the console (else BGR8).
            u16 fb_w = 0, fb_h = 0;
            const uint8_t* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fb_w, &fb_h);
            const bool rgb565 = gfxGetScreenFormat(GFX_BOTTOM) == GSP_RGB565_OES;
            if (fb != nullptr && fb_w == 240) {
                job->file[2] = make_bmp(fb_h, 240, &job->bytes[2], [fb, rgb565](int x, int y, uint8_t* rgb) {
                    const size_t i = (size_t)x * 240 + (239 - y);
                    if (rgb565) {
                        const u16 v = ((const u16*)fb)[i];
                        rgb[0] = (uint8_t)(((v >> 11) & 31) * 255 / 31); rgb[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63); rgb[2] = (uint8_t)((v & 31) * 255 / 31);
                    }
                    else { rgb[0] = fb[i * 3 + 2]; rgb[1] = fb[i * 3 + 1]; rgb[2] = fb[i * 3]; }
                });
            }
        }
        for (int i = 0; i < 3; i++) {
            char name[96];
            snprintf(name, sizeof(name), "screenshots/%s%s", g_shot_name, suffix[i]);
            rt64_3ds::data_path(job->path[i], sizeof(job->path[i]), name);
        }
        if (job->file[0] == nullptr) {
            fprintf(stderr, "rt64-3ds: screenshot: no memory for the image\n");
            for (int i = 0; i < 3; i++) free(job->file[i]);
            delete job;
            g_shot_state = 3;
            return;
        }
        g_shot_state = 1;
        if (threadCreate(write_shot, job, 32 * 1024, 0x2F, -2, true) == nullptr) write_shot(job);
    }

    static void describe_draw(uint32_t i, const rt64_3ds::DrawRecord& d) {
        fprintf(stderr, "rt64-3ds: draw %u: kind %d first %u count %u cc %06x %08x omh %06x oml %08x gm %06x prim %02x%02x%02x%02x env %02x%02x%02x%02x sc %d,%d-%d,%d",
                i, (int)d.kind, d.first, d.count, d.cc_w0, d.cc_w1, d.othermode_h, d.othermode_l, d.geometry_mode,
                d.prim[0], d.prim[1], d.prim[2], d.prim[3], d.env[0], d.env[1], d.env[2], d.env[3],
                d.scissor[0], d.scissor[1], d.scissor[2], d.scissor[3]);
        for (int t = 0; t < 2; t++) {
            if (d.tex[t].valid) {
                fprintf(stderr, " tex%d %06x/%u fmt %u siz %u %ux%u m%u/%u c%u/%u%s", t, d.tex[t].addr, d.tex[t].pitch, d.tex[t].fmt, d.tex[t].siz,
                        d.tex[t].width, d.tex[t].height, d.tex[t].masks, d.tex[t].maskt, d.tex[t].cms, d.tex[t].cmt, d.tex[t].snapshot ? " SNAP" : "");
            }
        }
        fprintf(stderr, " vp %.1f %.1f %.1f %.1f proj %.3f %.3f %.3f %.3f%s\n", d.dbg_vp[0], d.dbg_vp[1], d.dbg_vp[2], d.dbg_vp[3],
                d.dbg_proj[0], d.dbg_proj[1], d.dbg_proj[2], d.dbg_proj[3], d.perspective ? "" : " ortho");
    }

    // Texel i (in tiled order) of an eye target's colour buffer as RGB.
    void eye_texel(C3D_RenderTarget* t, uint32_t i, uint8_t* rgb) const {
        if (color16_) {
            const u16 p = ((const u16*)t->frameBuf.colorBuf)[i];
            rgb[0] = (uint8_t)(((p >> 11) & 31) * 255 / 31); rgb[1] = (uint8_t)(((p >> 5) & 63) * 255 / 63); rgb[2] = (uint8_t)((p & 31) * 255 / 31);
        }
        else {
            const uint32_t p = ((const uint32_t*)t->frameBuf.colorBuf)[i];
            rgb[0] = (uint8_t)(p >> 24); rgb[1] = (uint8_t)(p >> 16); rgb[2] = (uint8_t)(p >> 8);
        }
    }

    // cm is the tile's cms/cmt: bit 0 G_TX_MIRROR, bit 1 G_TX_CLAMP.
    static GPU_TEXTURE_WRAP_PARAM wrap_mode(uint8_t cm, uint8_t mask, uint16_t size) {
        if (mask == 0 || (cm & 2)) return GPU_CLAMP_TO_EDGE;
        if ((1u << mask) != size) return GPU_CLAMP_TO_EDGE;
        return (cm & 1) ? GPU_MIRRORED_REPEAT : GPU_REPEAT;
    }

    uint8_t* rdram_;
    rt64_3ds::Interpreter interp_;
    rt64_3ds::TextureCache textures_;
    rt64_3ds::FrameRecord frame_;
    bool ok_ = false;
    C3D_RenderTarget* top_ = nullptr;
    bool fb_capture_pending_ = false;
    uint32_t prev_depth_image_ = 0;

    // The finished frame's depth (the GPU is idle after C3D_FrameBegin) into
    // the game's screen-sized RDRAM depth buffer in the RDP's format: one sample
    // per 4x4 block, read straight from VRAM (8x8 Morton tiles, the target
    // rotated: row = screen x, column = 239 - screen y). Stored depth is
    // 1 - z' with z' = (clip z / w + 1) / 2; the RDP's 18-bit z is
    // z' * 0x3FE00 (viewport z scale and offset 0x1FF, x32, 15.3), stored
    // as 3-bit exponent (leading ones) and 11-bit mantissa, dz 0.
    static uint16_t n64_depth(float zp) {
        if (zp < 0.0f) zp = 0.0f;
        if (zp > 1.0f) zp = 1.0f;
        uint32_t z = (uint32_t)(zp * 261632.0f);
        if (z > 0x3FFFF) z = 0x3FFFF;
        uint32_t e, m;
        if (z < 0x20000) { e = 0; m = z >> 6; }
        else if (z < 0x30000) { e = 1; m = z >> 5; }
        else if (z < 0x38000) { e = 2; m = z >> 4; }
        else if (z < 0x3C000) { e = 3; m = z >> 3; }
        else if (z < 0x3E000) { e = 4; m = z >> 2; }
        else if (z < 0x3F000) { e = 5; m = z >> 1; }
        else if (z < 0x3F800) { e = 6; m = z; }
        else { e = 7; m = z; }
        return (uint16_t)((e << 13) | ((m & 0x7FF) << 2));
    }
    // One depth texel of the finished left-eye frame as z' (1 = cleared).
    // sx, sy on the 400x240 screen.
    float read_depth_zp(int sx, int sy) const {
        if (sx < 0) sx = 0; if (sx > 399) sx = 399;
        if (sy < 0) sy = 0; if (sy > 239) sy = 239;
        const int r = sx, c = 239 - sy;
        const uint32_t tile = (uint32_t)(r >> 3) * 30 + (uint32_t)(c >> 3);
        const uint32_t mo = (c & 1) | ((r & 1) << 1) | ((c & 2) << 1) | ((r & 2) << 2) | ((c & 4) << 2) | ((r & 4) << 3);
        const void* buf = depth_read_ != nullptr ? depth_read_ : top_->frameBuf.depthBuf;
        if (depth16_) {
            // RenderDesc::eye_depth16.
            const uint32_t d = ((const u16*)buf)[tile * 64 + mo];
            return 1.0f - (float)d * (1.0f / 65535.0f);
        }
        const uint32_t d = ((const uint32_t*)buf)[tile * 64 + mo] & 0xFFFFFF;
        return 1.0f - (float)d * (1.0f / 16777215.0f);
    }
    // The last game frame's depth on the grid (RenderDesc::depth_to_rdram
    // and the stereo statistics), and with `rdram` in the game's depth
    // buffer.
    void read_game_depth(bool rdram) {
        // The last game frame's copy (written by the GPU: dropped from
        // this core's cache first), else its eye in VRAM.
        if (depth_copy_ != nullptr && depth_copy_ready_) {
            svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)depth_copy_, depth_copy_bytes_);
            depth_read_ = depth_copy_;
        }
        else depth_read_ = nullptr;
        read_depth_grid();
        // Copies checked now and then against the eye itself, which
        // still holds the same frame (the first 8, every 300th frame).
        if (depth_read_ != nullptr && depth_checks_ < 8 && double_eyes_ && frames_ % 300 == 0) {
            depth_checks_++;
            float grid[kGridColsMax * kGridRowsMax];
            memcpy(grid, depth_grid_, sizeof(grid));
            depth_read_ = nullptr;
            read_depth_grid();
            int differ = 0, drawn = 0;
            for (int k = 0; k < grid_cols_ * grid_rows_; k++) { differ += grid[k] != depth_grid_[k]; drawn += grid[k] < 1.0f; }
            fprintf(stderr, "rt64-3ds: depth copy check %u: %d of %d grid texels differ from VRAM (%d with something drawn)\n",
                    depth_checks_, differ, grid_cols_ * grid_rows_, drawn);
            memcpy(depth_grid_, grid, sizeof(grid));
            depth_read_ = depth_copy_;
        }
        if (rdram) depth_to_rdram(prev_depth_image_);
    }
    // RenderDesc::early_drawn with depth_to_rdram: the last game frame's depth
    // into RDRAM once the presenter is done with that frame (its draws and
    // its depth copy are on the GPU's side of it), then "drawn": the game
    // goes on with its next frame only once its depth buffer holds it. The
    // grid serves this list's game frame (its stereo statistics) too.
    void early_depth() {
        if (!drawn_after_depth_) return;
        drawn_after_depth_ = false;
        const u64 td = svcGetSystemTick();
        if (prev_depth_image_ != 0) {
            read_game_depth(true);
            depth_read_early_ = true;
        }
        acc_depth_ += svcGetSystemTick() - td;
        ultramodern::renderer::report_list_drawn();
    }
    // The grid: one texel per 8x8 block of the game's frame, at the block
    // centre (VRAM reads by the CPU are slow on the console).
    void read_depth_grid() {
        for (int gy = 0; gy < grid_rows_; gy++) {
            for (int gx = 0; gx < grid_cols_; gx++) {
                const float nx = (float)(gx * 8 + 4), ny = (float)(gy * 8 + 4);
                depth_grid_[gy * grid_cols_ + gx] = read_depth_zp((int)geo_.screen_x(nx), (int)geo_.screen_y(ny));
            }
        }
    }
    void depth_to_rdram(uint32_t zimg) {
        const uint32_t pitch = (uint32_t)geo_.w * 2;
        if ((zimg & 0xFFFFFF) + pitch * (uint32_t)geo_.h > 16u * 1024 * 1024) return;
        uint8_t* z = rdram_ + (zimg & 0xFFFFFF);
        // A block is one value, so a word holds a pixel pair whichever half
        // each pixel is in (RDRAM's halves are swapped, ^2): word stores, a
        // quarter of the stores pixel by pixel took (needs an aligned buffer
        // and an even width; else pixel by pixel).
        const bool words = (zimg & 3) == 0 && (pitch & 3) == 0;
        for (int gy = 0; gy < grid_rows_; gy++) {
            for (int gx = 0; gx < grid_cols_; gx++) {
                // Nothing drawn there: the RDP's clear value (0xFFFC), which
                // the game reads as "nothing here" (its camera and the sun's
                // visibility test compare against it); the far plane itself
                // encodes lower (0xF800).
                const float zp = depth_grid_[gy * grid_cols_ + gx];
                const uint16_t v = zp >= 1.0f ? 0xFFFC : n64_depth(zp);
                const uint32_t vv = (uint32_t)v | ((uint32_t)v << 16);
                const int x0 = gx * 8, x1 = std::min(x0 + 8, geo_.w);
                for (int y = gy * 8; y < gy * 8 + 8 && y < geo_.h; y++) {
                    uint8_t* row = z + (uint32_t)y * pitch;
                    int x = x0;
                    if (words) for (; x + 1 < x1; x += 2) *(uint32_t*)(row + x * 2) = vv;
                    for (; x < x1; x++) *(uint16_t*)(row + ((x * 2) ^ 2)) = v;
                }
            }
        }
    }

    // osViBlack: the runtime zeroes VI_H_START while the game has the video
    // blanked, and RT64 then shows nothing. DK64 blanks while it draws the
    // frames of a new scene that its zipper transition stores and zips in;
    // presenting them flashed the new scene between unzip and zip. Those
    // frames still render into the eye target (a store copies it), but the
    // screen output is switched to a black target.
    void update_vi_blank() {
        const ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
        const bool blanked = vi != nullptr && vi->VI_H_START_REG == 0 && vi->VI_V_START_REG != 0;
        if (blanked == vi_blanked_ || black_ == nullptr) return;
        vi_blanked_ = blanked;
        fprintf(stderr, "rt64-3ds: video %s at frame %u\n", blanked ? "blanked" : "shown", frames_);
        if (blanked) {
            C3D_RenderTargetSetOutput(black_, GFX_TOP, GFX_LEFT, transfer_flags_);
            if (top_r_ != nullptr) C3D_RenderTargetSetOutput(nullptr, GFX_TOP, GFX_RIGHT, transfer_flags_);
            linked_set_ = -1;
        }
        else {
            C3D_RenderTargetSetOutput(top_, GFX_TOP, GFX_LEFT, transfer_flags_);
            if (top_r_ != nullptr) C3D_RenderTargetSetOutput(top_r_, GFX_TOP, GFX_RIGHT, transfer_flags_);
            linked_set_ = 0;
        }
    }

    // ---- stereo (see rt64_3ds_stereo.h)
    rt64_3ds::StereoFrame begin_stereo_frame() {
        rt64_3ds::StereoFrame sf;
        const rt64_3ds::Settings& st = rt64_3ds::settings();
        slider_ = osGet3DSliderState();
        sf.sep = (float)st.sep_slider * 0.002f * slider_ * st.slider_gain;
        sf.on = top_r_ != nullptr && slider_ > 0.001f && sf.sep > 0.0f && !vi_blanked_;
        if (sf.on != s3d_on_) {
            gfxSet3D(sf.on);
            s3d_on_ = sf.on;
        }
        stereo_wants_depth_ = sf.on;
        return sf;
    }

    // hold: an in-between frame (frame interpolation) keeps the last frame's
    // convergence and what it measured; the game's frames move them on.
    rt64_3ds::StereoFrame finish_stereo_frame(rt64_3ds::StereoFrame sf, bool hold = false) {
        const rt64_3ds::Settings& st = rt64_3ds::settings();
        if (hold) {
            sf.conv = held_sf_.conv > 0.0f ? held_sf_.conv : (float)st.convergence_hundredths * 0.2f;
            sf.world_empty = sf.on && held_sf_.world_empty;
            if (sf.on && st.hud_depth != 50) rt64_3ds::set_hud_terms(sf, st.hud_depth, slider_, st.slider_gain);
            if (rt64_3ds::render_desc().snap_hud_shift) sf.px_scale = geo_.scale;
            sf.reticle = sf.on && held_sf_.reticle;
            sf.aim_ndc = held_sf_.aim_ndc;
            return sf;
        }
        const float manual = (float)st.convergence_hundredths * 0.2f;
        sf.conv = manual;
        near_z_ = -1.0f;
        if (sf.on && st.auto_convergence) {
            if (prev_cam_world_) {
                const float zp = rt64_3ds::near_depth_statistic(depth_grid_, grid_cols_, grid_rows_, rt64_3ds::render_desc().autoconv.region_bottom);
                near_z_ = zp > 0.0f ? rt64_3ds::view_depth(zp, prev_m22_, prev_m32_) : -1.0f;
            }
            sf.conv = autoconv_.update(near_z_, manual, sf.sep, st.comfort_target, rt64_3ds::render_desc().autoconv);
        }
        else {
            autoconv_.reset();
        }
        applied_conv_ = sf.conv;
        if (sf.on) {
            bool any = false;
            for (int i = 0; i < grid_cols_ * grid_rows_ && !any; i++) any = depth_grid_[i] > 0.0f && depth_grid_[i] < 1.0f;
            sf.world_empty = !any;
        }
        if (sf.on && st.hud_depth != 50) rt64_3ds::set_hud_terms(sf, st.hud_depth, slider_, st.slider_gain);
        if (rt64_3ds::render_desc().snap_hud_shift) sf.px_scale = geo_.scale;
        sf.reticle = sf.on && rt64_3ds::first_person_scene();
        aim_z_ = -1.0f;
        if (!sf.reticle) aim_have_prev_ = false;
        if (sf.reticle) {
            aim_z_ = sample_aim_depth(sf);
            // Zero at the convergence distance, the full separation at
            // infinity (also when nothing was sampled: open sky), negative
            // nearer; clamped harder behind the screen than in front.
            const float ratio = aim_z_ > 0.0f ? sf.conv / aim_z_ : 0.0f;
            const float off = sf.sep * (1.0f - ratio);
            const float limit = off > 0.0f ? 0.10f : 0.30f;
            sf.aim_ndc = off < -limit ? -limit : off > limit ? limit : off;
        }
        held_sf_ = sf;
        return sf;
    }

    // The depth under the reticle: the desktop's aim strip (RT64's
    // StereoDepthSampler and its consumer in the workload queue), read from
    // the left eye's finished frame.
    //
    // That eye is not a centre view: content at fused position u and view
    // depth z sits at u + s*(conv/z - 1) in it (s = its separation in
    // pixels). So candidate patches are read across every position the aim
    // point could resolve to, from -s (infinity) to +2s (a third of the
    // convergence distance), and each patch's median depth predicts where
    // that patch would have to sit. Those within one patch spacing of their
    // prediction are consistent, and the NEAREST consistent one wins: at an
    // occlusion edge both surfaces can be consistent and the near one is what
    // is visible. An empty patch is cleared depth, the far plane: an infinity
    // candidate whose position is known exactly (-s). With nothing consistent
    // the previous answer holds (a guess only when there is none), and the
    // result is smoothed in z behind a 1% relative deadband.
    //
    // The prediction uses the separation and convergence that frame was
    // drawn with. Depth needs no viewport correction here (the desktop's
    // G_MAXZ term): the interpreter writes z' = (z + w) / 2 itself, so
    // 2 z' - 1 is exactly the projection's ndc.z.
    float sample_aim_depth(const rt64_3ds::StereoFrame& sf) {
        (void)sf;
        if (!prev_cam_world_) { aim_have_prev_ = false; return -1.0f; }
        constexpr int kCandidates = 13, kPatch = 5, kMinSamples = 2;
        const float s_px = prev_sep_ * 200.0f, conv = prev_conv_;
        const bool shifted = s_px > 0.5f && conv > 0.0f;
        const float lo = shifted ? -s_px : 0.0f, hi = shifted ? 2.0f * s_px : 0.0f;
        const float spacing = (hi - lo) / (float)(kCandidates - 1);
        const float tolerance = std::max(spacing, (float)kPatch * 0.5f);
        const int cx = (int)lroundf(geo_.screen_x(reticle_cx_)), cy = (int)lroundf(geo_.screen_y(reticle_cy_));
        float aim_inv = -1.0f, best_res = 1e9f, best_inv = -1.0f;
        for (int i = 0; i < (shifted ? kCandidates : 1); i++) {
            const float o = lo + spacing * (float)i;
            const int px = cx + (int)lroundf(o);
            float zs[kPatch * kPatch];
            int n = 0;
            for (int dy = -kPatch / 2; dy <= kPatch / 2; dy++) {
                for (int dx = -kPatch / 2; dx <= kPatch / 2; dx++) {
                    const float zp = read_depth_zp(px + dx, cy + dy);
                    if (zp > 0.0f && zp < 1.0f) zs[n++] = zp;
                }
            }
            float inv = 0.0f, predicted = lo;
            if (n >= kMinSamples) {
                std::nth_element(zs, zs + n / 2, zs + n);
                const float z = rt64_3ds::view_depth(zs[n / 2], prev_m22_, prev_m32_);
                if (z <= 0.0f) continue;
                inv = 1.0f / z;
                predicted = shifted ? s_px * (conv * inv - 1.0f) : 0.0f;
            }
            const float res = std::fabs(predicted - o);
            if (res <= tolerance) {
                if (inv > aim_inv) aim_inv = inv;      // nearest; infinity (0) loses to any surface
            }
            else if (res < best_res) {
                best_res = res;
                best_inv = inv;
            }
        }
        if (aim_inv < 0.0f && !aim_have_prev_) aim_inv = best_inv;
        if (aim_inv < 0.0f) return aim_have_prev_ ? aim_ema_ : -1.0f;   // hold
        if (aim_inv <= 0.0f) {
            // Infinity: nothing finite to ease toward.
            aim_have_prev_ = false;
            aim_ema_ = -1.0f;
            return -1.0f;
        }
        const float z = 1.0f / aim_inv;
        if (!aim_have_prev_) aim_ema_ = z;
        else if (std::fabs(z - aim_ema_) / aim_ema_ > 0.01f) aim_ema_ += (z - aim_ema_) * 0.25f;
        aim_have_prev_ = true;
        return aim_ema_;
    }

    // Each draw's treatment for this frame, and where the reticle is (for
    // the next frame's depth sample).
    void classify_frame(const rt64_3ds::StereoFrame& sf) {
        classes_.resize(frame_.draws.size());
        memset(class_count_, 0, sizeof(class_count_));
        const rt64_3ds::RenderDesc& desc = rt64_3ds::render_desc();
        // RenderDesc::hud_camera_id: the HUD camera is never the frame's main
        // world projection (the desktop fork's guard: the full-width
        // perspective with the most draws is the world, whatever its camera
        // looks like). Judged on this frame's draws; when it holds, the HUD
        // camera's draws are the world's and its projection the depth camera.
        primary_inst_ = 0;
        if (desc.primary_world_instance) {
            // RenderDesc::primary_world_instance: one projection instance is
            // the world (the desktop's primaryWorld); a HUD-camera one takes
            // the depth camera with it.
            bool hud = false;
            primary_inst_ = rt64_3ds::primary_world_instance(frame_.draws.data(), frame_.draws.size(), desc, geo_.w, &hud,
                                                             inst_table_, kInstTable, &inst_table_n_);
            hud_cam_is_world_ = hud;
            for (int k = 0; k < inst_table_n_ && k < kInstTable; k++) {
                if (inst_table_[k].proj_id == desc.hud_camera_id) { hudcam_frames_++; break; }
            }
            if (hud) hudcam_world_frames_++;
            if (hud) {
                for (const rt64_3ds::FrameRecord::InstanceCam& c : frame_.inst_cams) {
                    if (c.inst != primary_inst_) continue;
                    memcpy(frame_.cam, c.m, sizeof(frame_.cam));
                    frame_.has_cam = true;
                    frame_.cam_is_world = true;
                    break;
                }
            }
        }
        else {
            hud_cam_is_world_ = rt64_3ds::hud_camera_is_world(frame_.draws.data(), frame_.draws.size(), desc, geo_.w);
            if (hud_cam_is_world_ && frame_.has_hud_cam) {
                memcpy(frame_.cam, frame_.hud_cam, sizeof(frame_.cam));
                frame_.has_cam = true;
                frame_.cam_is_world = true;
            }
        }
        if (!sf.on) return;
        const rt64_3ds::Vtx3ds* verts = frame_.verts.data();
        // RenderDesc::infinity_rect_cover_zoom. The vertices are recorded
        // once and replayed for both eyes, so the zoom (eye-independent) is
        // applied to them here, before they are flushed to the GPU.
        const float cover = rt64_3ds::cover_zoom_factor(sf.sep, geo_);
        bool any = false;
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (size_t i = 0; i < frame_.draws.size(); i++) {
            const rt64_3ds::DrawRecord& d = frame_.draws[i];
            rt64_3ds::StereoClass cls = rt64_3ds::classify_draw(d, desc);
            if (hud_cam_is_world_ && d.kind == rt64_3ds::DrawRecord::Tris && d.perspective && d.proj_id == desc.hud_camera_id &&
                (!desc.primary_world_instance || d.proj_inst == primary_inst_)) {
                cls = rt64_3ds::StereoClass::World;
            }
            if (cls == rt64_3ds::StereoClass::Infinity && d.kind != rt64_3ds::DrawRecord::Tris && desc.infinity_rect_cover_zoom && d.rtt < 0) {
                rt64_3ds::cover_zoom(d, frame_.verts.data(), cover, geo_);
            }
            if (cls == rt64_3ds::StereoClass::Rect && rt64_3ds::rect_covers_scissor(d, verts)) {
                // Full-screen tints stay on the glass; with
                // RenderDesc::covering_rects_hud only those covering the
                // whole frame (a panel filling its own scissor is HUD).
                if (!desc.covering_rects_hud || rt64_3ds::rect_covers_frame(d, verts, geo_.w, geo_.h)) cls = rt64_3ds::StereoClass::ScreenOverlay;
                else covering_hud_++;
            }
            if (cls == rt64_3ds::StereoClass::World && sf.world_empty) cls = rt64_3ds::StereoClass::Hud;
            uint8_t c = (uint8_t)cls;
            float a, b, e, f;
            if (sf.reticle && !d.perspective && cls != rt64_3ds::StereoClass::Infinity &&
                rt64_3ds::is_reticle_quad(d, verts, &a, &b, &e, &f)) {
                c = kReticle;
                any = true;
                x0 = std::min(x0, a); x1 = std::max(x1, b); y0 = std::min(y0, e); y1 = std::max(y1, f);
            }
            classes_[i] = c;
            class_count_[c == kReticle ? 7 : (c < 7 ? c : 0)]++;
        }
        if (any) { reticle_cx_ = (x0 + x1) * 0.5f; reticle_cy_ = (y0 + y1) * 0.5f; }
        else { reticle_cx_ = (float)geo_.w * 0.5f; reticle_cy_ = (float)geo_.h * 0.5f; }
    }

    static constexpr uint8_t kReticle = 0xFF;
    // The depth grid: 8x8 blocks of the game's frame (40x30 for 320x240).
    static constexpr int kGridColsMax = 50, kGridRowsMax = 30;
    rt64_3ds::ScreenGeometry geo_ = rt64_3ds::ScreenGeometry::make(320, 240);
    int grid_cols_ = 40, grid_rows_ = 30;
    int draw_lo_ = 0, draw_hi_ = 1 << 30;       // DRAW_RANGE.TXT

    // Decode (or find in the cache) every RDRAM texture the frame's draws
    // use, so the replay reads nothing more from RDRAM.
    // What each draw's RDRAM textures are bound as (bound_[2 * draw + t]):
    // the replay of both eyes takes them from here instead of looking each
    // up again (a fingerprint of 16 RDRAM words and a hash lookup per draw
    // and eye). Nothing is decoded or freed between here and the frame's
    // end, so the textures stay where they are.
    std::vector<rt64_3ds::BoundTex> bound_;
    void prefetch_textures() {
        bound_.assign(frame_.draws.size() * 2, rt64_3ds::BoundTex{});
        for (size_t i = 0; i < frame_.draws.size(); i++) {
            const rt64_3ds::DrawRecord& d = frame_.draws[i];
            for (int t = 0; t < 2; t++) {
                const rt64_3ds::TexDesc& td = d.tex[t];
                if (!td.valid || td.snapshot || td.frame || td.rtt >= 0) continue;
                const uint16_t* pal = (td.fmt == 2 && td.tlut_index + 256 <= frame_.tlut.size()) ? &frame_.tlut[td.tlut_index] : nullptr;
                const rt64_3ds::TexDesc* mips = (td.levels > 1 && td.mip_index + td.levels - 1 <= frame_.mips.size()) ? &frame_.mips[td.mip_index] : nullptr;
                rt64_3ds::BoundTex b = textures_.get(td, pal, mips);
                // Every texture is in the cache before the replay starts and
                // it binds from white_ at the frame's start: no address of a
                // texture freed during the replay to tell apart.
                b.fresh = false;
                bound_[i * 2 + t] = b;
            }
        }
    }

    // Render to texture: one target per off-screen image slot
    // (FrameRecord::rtts), a VRAM texture with its own depth buffer, drawn
    // in the image's N64 pixels (y down) and sampled as a texture.
    struct RttTarget {
        C3D_Tex tex{};
        C3D_RenderTarget* rt = nullptr;
        uint16_t w = 0, h = 0;
        C3D_Mtx proj{};
        uint32_t clear_frame = ~0u;
    };
    RttTarget rtt_[rt64_3ds::kMaxRtt];
    uint32_t rtt_room_frame_ = ~0u - 8;          // the texture cache last gave VRAM back for a target
    int cur_target_ = -1;                        // the RTT slot drawn into, -1 = the eye
    RttTarget* ensure_rtt(int i) {
        const rt64_3ds::RttImage& img = frame_.rtts[i];
        RttTarget& t = rtt_[i];
        if (t.rt == nullptr || t.w != img.pot_w || t.h != img.pot_h) {
            // Deleting a render target inside a frame is a citro3d PANIC (and
            // the GPU may still read it): retire it until the next frame start.
            if (t.rt != nullptr) { retired_rtt_.push_back({ t.rt, t.tex }); t.rt = nullptr; }
            if (img.pot_w == 0 || img.pot_h == 0) return nullptr;
            // Short of VRAM (textures took it): the texture cache gives some
            // back (free three frames on) and keeps it free; again every four
            // frames the target still fails.
            const uint32_t need = (uint32_t)img.pot_w * img.pot_h * 8;   // RGBA8 + D24S8
            if (!C3D_TexInitVRAM(&t.tex, img.pot_w, img.pot_h, GPU_RGBA8)) {
                if (frames_ - rtt_room_frame_ >= 4) { textures_.make_vram_room(need); rtt_room_frame_ = frames_; }
                fprintf(stderr, "rt64-3ds: no VRAM for a %ux%u render texture\n", (unsigned)img.pot_w, (unsigned)img.pot_h);
                return nullptr;
            }
            t.rt = C3D_RenderTargetCreateFromTex(&t.tex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
            if (t.rt == nullptr) {
                C3D_TexDelete(&t.tex);
                if (frames_ - rtt_room_frame_ >= 4) { textures_.make_vram_room(need); rtt_room_frame_ = frames_; }
                fprintf(stderr, "rt64-3ds: render target for a %ux%u texture failed\n", (unsigned)img.pot_w, (unsigned)img.pot_h);
                return nullptr;
            }
            t.w = img.pot_w; t.h = img.pot_h;
            C3D_TexSetFilter(&t.tex, GPU_LINEAR, GPU_LINEAR);
            C3D_TexSetWrap(&t.tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            // N64 image pixels, y down: the image's top row lands at the
            // texture's top (t = 1), where the interpreter's texel
            // coordinates count from (pot_h - y).
            Mtx_Ortho(&t.proj, 0.0f, (float)t.w, (float)t.h, 0.0f, 0.0f, 1.0f, true);
            fprintf(stderr, "rt64-3ds: render texture %d for image %06x (%u wide): %ux%u (frame %u)\n", i, img.addr & 0xFFFFFF,
                    (unsigned)img.width, (unsigned)t.w, (unsigned)t.h, frames_);
        }
        if (t.clear_frame != frames_) {
            static int logged = 0;
            if (logged < 60) { logged++; fprintf(stderr, "rt64-3ds: frame %u draws into render texture %d\n", frames_, i); }
            // First draw into it this frame: a fresh depth buffer (the
            // colour is the game's own fill).
            clear_target(t.rt, C3D_CLEAR_ALL, 0x000000FF);
            t.clear_frame = frames_;
        }
        return &t;
    }
    struct RetiredRtt { C3D_RenderTarget* rt; C3D_Tex tex; };
    std::vector<RetiredRtt> retired_rtt_;
    // Out of the frame, just before C3D_FrameBegin (which would wait for the
    // GPU anyway): render targets ensure_rtt replaced.
    void retire_rtt_targets() {
        for (RetiredRtt& r : retired_rtt_) { C3D_RenderTargetDelete(r.rt); C3D_TexDelete(&r.tex); }
        retired_rtt_.clear();
    }

    // RenderDesc::sample_drawn_frame: a copy of the eye's frame as drawn so
    // far, 256x512 with the eye's 240x400 kFrameRowOffset rows up, where a
    // TextureCopy of the eye puts it (the snapshot's layout: texture s =
    // 240 - screen y, t = 512 - screen x).
    static constexpr int kFrameTarget = -3;
    static constexpr u32 kFrameRowOffset = 512 - 400;
    C3D_Tex frame_tex_{};
    C3D_RenderTarget* frame_rt_ = nullptr;
    bool frame_viewport_ = false;              // drawing into frame_rt_
    uint32_t fc_frames_ = 0, fc_samples_ = 0, fc_image_draws_ = 0, fc_mirrored_ = 0, frames_at_fc_ = 0;

    // Draw into target tgt from here on: >= 0 an off-screen image, -1 the
    // eye of this pass, kFrameTarget the eye's frame copy.
    bool bind_target(int tgt, int pass) {
        if (tgt >= 0) {
            RttTarget* t = ensure_rtt(tgt);
            if (t == nullptr) return false;
            draw_on(t->rt);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_xform_, &t->proj);
        }
        else if (tgt == kFrameTarget) {
            if (frame_rt_ == nullptr) return false;
            draw_on(frame_rt_);
            // Framebuffer y is texture t in a target made from a texture.
            C3D_SetViewport(0, kFrameRowOffset, 240, 400);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_xform_, &proj_);
        }
        else {
            draw_on(cur_eye_[pass == 0 ? 0 : 1]);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_xform_, &proj_);
        }
        frame_viewport_ = tgt == kFrameTarget;
        cur_target_ = tgt;
        misc_valid_ = false;       // the scissor and write mask belong to the target
        return true;
    }

    C3D_RenderTarget* top_r_ = nullptr;
    C3D_RenderTarget* black_ = nullptr;
    // ---- RenderDesc::double_eyes (init_double_eyes, begin_deferred,
    // present_deferred)
    C3D_RenderTarget* inb_l_ = nullptr;         // the in-betweens' eyes
    C3D_RenderTarget* inb_r_ = nullptr;
    bool double_eyes_ = false;
    C3D_RenderTarget* cur_eye_[2] = {};         // the eyes this frame draws into (top_/top_r_, or the in-betweens')
    C3D_RenderTarget* last_eye_[2] = {};        // ... and the last frame drawn's (captures, screenshots and copies read them)
    int linked_set_ = 0;                        // what citro3d transfers to the screen: 0 top_/top_r_, 1 the in-betweens', -1 black_
    bool pair_ = false;                         // send_dl: replaying an interpolated pair
    static constexpr u32 kDeferBytes = C3D_DEFAULT_CMDBUF_SIZE * 2;   // as citro3d's own (C3D_Init above)
    struct DeferredOp {                         // in GX queue order: a command list, a clear (fill), or a copy
        u32* list;
        u32 words;
        C3D_RenderTarget* fill;
        C3D_ClearBits bits;
        u32 color;
        const void* copy_src = nullptr;         // a copy: copy_bytes from copy_src to copy_dst (copy_depth)
        void* copy_dst = nullptr;
        u32 copy_bytes = 0;
    };
    static constexpr int kMaxDeferredOps = 30;  // citro3d's GX queue takes 32 a frame
    u32* dbuf_ = nullptr;                       // the game frame's command buffer (linear)
    DeferredOp dops_[kMaxDeferredOps] = {};
    int dop_count_ = 0;
    bool deferring_ = false, dops_overflow_ = false;
    u32 dbuf_used_max_ = 0;                     // words, this report
    // The presenter's job: set before present_go_, read back after present_done_.
    bool job_deferred_ = false, job_stereo_ = false;
    bool job_inb_ = false, job_inb_stereo_ = false;   // the presenter transfers a drawn in-between first
    bool inb_pending_ = false, inb_stereo_ = false;   // gfx thread: an in-between drawn, its transfer to come
    uint32_t held_presented_ = 0, held_flushed_ = 0;   // in-betweens transferred by the presenter / here (report)
    const void* inb_fb_ = nullptr;              // the back buffer the in-between was transferred to
    LightEvent inb_free_{};                     // the in-between's GPU work is done (vbo_[2] may be refilled)
    float job_inb_gpu_ms_ = 0.0f, job_draw_gpu_ms_ = 0.0f;
    float job_inb_draw_ms_ = 0.0f;              // the presenter: the in-between's draws, before its transfer
    u64 job_wait_inb_ = 0, job_wait_draws_ = 0;
    int dinb_kind_ = -1, dinb_eyes_ = 0;        // the in-between's GPU time pending, for the presenter to read
    bool dinb_fill_ = false;
    float gpu_pending_extra_ms_ = 0.0f;         // the pending frame's draws (a frame of their own), read by the presenter
    // The report: the pairs' GPU waits on the gfx thread ([0] the game's
    // frame, [1] the in-between), the presenter's, the in-between vertex
    // buffer's.
    u64 acc_pair_wait_[2] = {}, acc_presenter_gpu_wait_[2] = {}, acc_vbo_wait_ = 0;
    uint32_t pair_frames_ = 0, deferred_frames_ = 0;

    // RenderDesc::double_eyes (Settings::gpu_double_eyes): the in-betweens'
    // eye targets and the command buffer the game's frame of a pair is
    // recorded into. Taken after everything else that wants VRAM (the
    // snapshot, the frame copy target); without them, or without the
    // presenter thread, the single set as ever.
    void init_double_eyes(GPU_COLORBUF color, GPU_DEPTHBUF depth) {
        const u32 free0 = vramSpaceFree();
        const u32 need = (top_r_ != nullptr ? 2 : 1) * (C3D_CalcColorBufSize(240, 400, color) + C3D_CalcDepthBufSize(240, 400, depth));
        const char* why = nullptr;
        if (presenter_ == nullptr) why = "no presenter thread";
        else if ((dbuf_ = (u32*)linearAlloc(kDeferBytes)) == nullptr) why = "no linear memory for the command buffer";
        else {
            inb_l_ = C3D_RenderTargetCreate(240, 400, color, depth);
            if (inb_l_ != nullptr && top_r_ != nullptr) inb_r_ = C3D_RenderTargetCreate(240, 400, color, depth);
            if (inb_l_ == nullptr || (top_r_ != nullptr && inb_r_ == nullptr)) {
                if (inb_l_ != nullptr) C3D_RenderTargetDelete(inb_l_);
                inb_l_ = nullptr;
                why = "no VRAM for the in-betweens' eye targets";
            }
        }
        if (why != nullptr) {
            if (dbuf_ != nullptr) linearFree(dbuf_);
            dbuf_ = nullptr;
            fprintf(stderr, "rt64-3ds: double eyes OFF: %s (VRAM free %u KB, %u KB needed)\n", why, (unsigned)(free0 / 1024), (unsigned)(need / 1024));
            return;
        }
        double_eyes_ = true;
        LightEvent_Init(&inb_free_, RESET_STICKY);
        LightEvent_Signal(&inb_free_);
        fprintf(stderr, "rt64-3ds: double eyes on: in-between eye targets %u KB (VRAM free %u -> %u KB), game-frame command buffer %u KB (linear free %u KB)\n",
                (unsigned)(need / 1024), (unsigned)(free0 / 1024), (unsigned)(vramSpaceFree() / 1024), (unsigned)(kDeferBytes / 1024),
                (unsigned)(linearSpaceFree() / 1024));
        start_builder();
    }

    // The screen's outputs follow the set the frame draws into (double eyes;
    // in a frame, where citro3d does not wait for the GPU to relink).
    void link_eyes(int set) {
        if (!double_eyes_ || vi_blanked_ || linked_set_ == set) return;
        C3D_RenderTargetSetOutput(set != 0 ? inb_l_ : top_, GFX_TOP, GFX_LEFT, transfer_flags_);
        if (top_r_ != nullptr) C3D_RenderTargetSetOutput(set != 0 ? inb_r_ : top_r_, GFX_TOP, GFX_RIGHT, transfer_flags_);
        linked_set_ = set;
    }

    // Recording the game's frame of a pair without a frame of citro3d's
    // (double eyes): the state calls work as ever, their commands go to
    // dbuf_; what citro3d would put on the GX queue (a clear, a split's
    // command list) is kept in dops_, in the same order, for
    // present_deferred.
    void begin_deferred() {
        dop_count_ = 0;
        GPUCMD_SetBuffer(dbuf_, kDeferBytes / 4, 0);
        deferring_ = true;
    }
    void end_deferred() {
        defer_split();
        const u32 used = (u32)(gpuCmdBuf - dbuf_);  // words: each split moved the buffer past its list
        if (used > dbuf_used_max_) dbuf_used_max_ = used;
        flush_dcache(dbuf_, used * 4);
        GPUCMD_SetBuffer(nullptr, 0, 0);
        deferring_ = false;
    }
    void push_op(const DeferredOp& op) {
        if (dop_count_ < kMaxDeferredOps) { dops_[dop_count_++] = op; return; }
        if (!dops_overflow_) {
            dops_overflow_ = true;
            fprintf(stderr, "rt64-3ds: double eyes: a frame of more than %d GX operations, the rest dropped\n", kMaxDeferredOps);
        }
    }
    void defer_split() {
        u32* list = nullptr;
        u32 words = 0;
        if (C3Di_SplitFrame(&list, &words)) push_op(DeferredOp{ list, words, nullptr, C3D_CLEAR_ALL, 0 });
    }
    void clear_target(C3D_RenderTarget* t, C3D_ClearBits bits, u32 color) {
        if (!deferring_) { C3D_RenderTargetClear(t, bits, color, 0); return; }
        // As citro3d queues a clear in a frame: at once, ahead of the
        // commands recorded since the last split.
        push_op(DeferredOp{ nullptr, 0, t, bits, color });
    }
    void draw_on(C3D_RenderTarget* t) {
        if (!deferring_) { C3D_FrameDrawOn(t); return; }
        // C3D_FrameDrawOn without its frame (and without marking the target
        // used: present_deferred marks the eyes for their transfer).
        C3D_SetFrameBuf(&t->frameBuf);
        C3D_SetViewport(0, 0, t->frameBuf.width, t->frameBuf.height);
    }
    void frame_split() {
        if (!deferring_) C3D_FrameSplit(GX_CMDLIST_FLUSH);   // frame ends flush only their own list
        else defer_split();
    }
    // The commands since the last split, as a command list of their own on
    // the GX queue (or the deferred frame's ops); keep_*: where it is, to
    // queue it again (queue_again). The kernel's flush on this core, as for
    // everything else the GPU reads.
    void close_segment(u32** keep_list = nullptr, u32* keep_words = nullptr) {
        u32* list = nullptr;
        u32 words = 0;
        if (!C3Di_SplitFrame(&list, &words)) return;
        if (deferring_) push_op(DeferredOp{ list, words, nullptr, C3D_CLEAR_ALL, 0 });
        else {
            flush_dcache(list, words * 4);
            GX_ProcessCommandList(list, words * 4, 0);
        }
        if (keep_list != nullptr) { *keep_list = list; *keep_words = words; }
    }
    void queue_again(u32* list, u32 words) {
        if (deferring_) push_op(DeferredOp{ list, words, nullptr, C3D_CLEAR_ALL, 0 });
        else GX_ProcessCommandList(list, words * 4, 0);    // flushed when it was first queued
    }
    // The game frame's left-eye depth to depth_copy_ (texture-copy mode,
    // rows of tiles as they are), after its draws on the GX queue: on the
    // queue now, or with the deferred frame's ops for the presenter.
    static void copy_depth(const void* src, void* dst, u32 bytes) {
        constexpr u32 kTextureCopyMode = BIT(3);
        const u32 dim = GX_BUFFER_DIM(240 * 8 * 2 / 16, 0);    // no gaps: any line length will do
        GX_TextureCopy((u32*)src, dim, (u32*)dst, dim, bytes, kTextureCopyMode);
    }
    void queue_depth_copy() {
        const void* src = cur_eye_[0]->frameBuf.depthBuf;
        if (deferring_) {
            DeferredOp op{ nullptr, 0, nullptr, C3D_CLEAR_ALL, 0 };
            op.copy_src = src;
            op.copy_dst = depth_copy_;
            op.copy_bytes = depth_copy_bytes_;
            push_op(op);
        }
        else copy_depth(src, depth_copy_, depth_copy_bytes_);
    }
    u8* depth_copy_ = nullptr;                  // the last game frame's depth (linear), or null: read VRAM
    u32 depth_copy_bytes_ = 0;
    bool depth_copy_ready_ = false;             // the last game frame queued its copy
    const void* depth_read_ = nullptr;          // what read_depth_zp reads this frame
    uint32_t depth_checks_ = 0;                 // copies checked against VRAM (the first few)
    // What apply_state and the shift cache: forgotten, so the next draw sets
    // all of its state itself.
    void forget_draw_state() {
        last_tex_[0] = last_tex_[1] = nullptr;
        last_tex_param_[0] = last_tex_param_[1] = 0xFFFFFFFFu;
        last_us_ = last_vs_ = -1.0f;
        last_uv1_[0] = -1.0f;
        last_cc_[0] = last_cc_[1] = 0xFFFFFFFFu;
        last_plan_.stages = -1;
        misc_valid_ = false;
        last_sa_ = last_sb_ = 1e30f;
    }
    bool eye_replay_on_ = true;                 // EYE_REPLAY_OFF.TXT: false
    uint32_t eye_frames_ = 0, eye_replays_ = 0; // frames drawn for two eyes / the right eye from the left's commands, this report
    // The eyes' formats (RenderDesc::eye_color16 / eye_depth16), their
    // display transfer to the screen and their clear value.
    bool color16_ = false, depth16_ = false;
    u32 transfer_flags_ = 0, eye_clear_ = 0x000000FFu;
    // GPU time (C3D_GetDrawingTime) per report, [0] game frames, [1]
    // in-betweens; the frame ended last waits to be read at the next
    // C3D_FrameBegin.
    float gpu_ms_[2] = {};
    uint32_t gpu_frames_[2] = {}, gpu_eyes_[2] = {};
    int gpu_pending_kind_ = -1, gpu_pending_eyes_ = 0;
    bool gpu_pending_fill_ = false;             // ... and whether it was cleared to its fill (fill_clear)
    bool vi_blanked_ = false;
    bool s3d_on_ = false, stereo_wants_depth_ = false;
    float slider_ = 0.0f;
    std::vector<uint8_t> classes_;
    uint32_t class_count_[8] = {};   // last frame's draws per StereoClass, [7] = reticle quads
    bool hud_cam_is_world_ = false;  // this frame's HUD camera is its world (RenderDesc::hud_camera_id)
    // RenderDesc::primary_world_instance: this frame's primary world
    // instance and its candidates, for the log.
    static constexpr int kInstTable = 8;
    uint32_t primary_inst_ = 0;
    rt64_3ds::InstanceCount inst_table_[kInstTable];
    int inst_table_n_ = 0;
    uint32_t covering_hud_ = 0;      // covering rectangles kept at the HUD's shift (RenderDesc::covering_rects_hud), this report
    uint32_t hudcam_frames_ = 0, hudcam_world_frames_ = 0;   // frames with a full-width HUD-camera instance / with it as the world, this report
    float last_sa_ = 0.0f, last_sb_ = 0.0f;
    u64 acc_left_ = 0, acc_right_ = 0;
    rt64_3ds::AutoConvergence autoconv_;
    float depth_grid_[kGridColsMax * kGridRowsMax] = {};
    bool prev_cam_world_ = false;
    float prev_m22_ = 0.0f, prev_m32_ = 0.0f;
    float reticle_cx_ = 160.0f, reticle_cy_ = 120.0f;
    float applied_conv_ = 0.0f, near_z_ = -1.0f, aim_z_ = -1.0f;
    float prev_sep_ = 0.0f, prev_conv_ = 0.0f;
    float aim_ema_ = -1.0f;
    bool aim_have_prev_ = false;
    int capture_snapshot_frames_ = 0;
    int capture_want_ = -2, store_delay_ = -1;
    bool capture_pair_ = false, capture_pair_next_ = false;   // GFX_CAPTURE.TXT "pair <frame>"
    bool rtt_capture_armed_ = false;            // GFX_CAPTURE.TXT "rtt"
    int seq_rtt_count_ = 0;                     // GFX_CAPTURE.TXT "seqrtt <count>"
    OSTask rtt_capture_task_{};
    int seq_first_ = 0, seq_count_ = 0;
    int cam_log_first_ = 0, cam_log_count_ = 0;
    bool cam_log_now_ = false;
    uint8_t* capture_copy_ = nullptr;
    OSTask capture_task_{};
    C3D_Tex white_{};
    C3D_Tex snapshot_{};
    uint32_t snapshots_ = 0;
    bool check_snapshot_ = false;
    bool gpu_debug_ = false, debug_sync_ = true;
    uint32_t debug_from_ = 0, debug_count_ = 0, debug_step_ = 1;
    DVLB_s* dvlb_ = nullptr;
    shaderProgram_s prog_{};
    int u_xform_ = -1, u_stereo_ = -1, u_eye_ = -1, u_uvscale_ = -1, u_uv1_ = -1;
    float last_uv1_[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
    C3D_Mtx proj_{};
    GpuVertex* vbo_[4] = { nullptr, nullptr, nullptr, nullptr };
    int vbo_idx_ = 0;
    // ---- frame interpolation (RenderDesc::frame_interpolation)
    bool interp_mode_ = false;                  // Settings::frame_cap 2: the interpreter records transforms
    bool engaged_ = false;                      // this list: the game asks for less than 60, frames are paced
    uint32_t prev_rate_ = 0;                    // the refresh rate the list before asked for
    bool prev_list_ok_ = false;                 // ... and whether it can be interpolated from
    rt64_3ds::FrameRecord inter_;               // the in-between frame
    std::vector<rt64_3ds::BoundTex> inter_bound_;
    rt64_3ds::StereoFrame held_sf_{};           // the last game frame's stereo terms (in-between frames keep them)
    rt64_3ds::interp::MatchResult last_match_{};
    u64 last_pair_end_ = 0;                     // when the last paced list was done
    bool resync_ = false;                       // the next list goes without an in-between (the presents ran late)
    int inb_count_ = 0;                         // in-betweens this list draws (make_inbetween): 1 at 30 fps, 2 at 20, 3 at 15
    u64 inb_built_ = 0;                         // building the in-betweens after the first, this list
    float pair_ms_ = 0.0f, pair_ema_ = 0.0f;    // what a pair (in-betweens + frame) costs, last and expected, without the waits
    float real_ema_ = 0.0f, inb_ema_ = 0.0f;    // ... of which the game's frame and the in-between
    float inb_share_ = 0.0f;                    // the in-between's cost as a share of the game frame's
    uint32_t budget_drops_ = 0;                 // lists that went without one for the budget since the last try
    // Trying in-betweens over budget (make_inbetween): lists left in this
    // run, lists to drop before the next, runs this report.
    static constexpr uint32_t kProbeLists = 8, kProbeEvery = 30, kProbeEveryMax = 240;
    uint32_t probe_left_ = 0, probe_every_ = kProbeEvery, probes_ = 0;
    uint32_t inb_snapshot_ = 0, inb_rtt_ = 0, inb_offscreen_ = 0, inb_blank_ = 0, inb_depth_ = 0;   // unsafe, by what
    uint32_t inb_after60_ = 0, inb_after_unsafe_ = 0;   // unrelated: the list before asked 60 / was unsafe
    u64 acc_vsync_ = 0, vsync_at_dl_ = 0, acc_inb_interp_ = 0, acc_inb_replay_ = 0, acc_match_ = 0;
    uint32_t presented_ = 0, inb_drawn_ = 0, inb_late_ = 0, inb_budget_ = 0, inb_unsafe_ = 0, inb_unrelated_ = 0;
    uint32_t inb_late_resync_ = 0;              // ... of inb_late_, dropped to resync with the screen (resync_)
    bool behind_resync_ = false;                // this list's `behind` came from resync_
    u64 acc_list_busy_ = 0, max_list_busy_ = 0; // the gfx thread's time per drawn list, dequeue to done
    u64 acc_list_read_ = 0, max_list_read_ = 0; // ... of which reading it (to "parsed": the game waits for that)
    float last_read_ms_ = 0.0f;
    float last_tris_ms_ = 0.0f;   // the list's deferred triangles (finish_triangles), on this thread
    u64 acc_list_wait_ = 0, max_list_wait_ = 0; // its wait for the gfx thread (queued behind the one before)
    u64 acc_build_wait_ = 0;                    // the builder: what of its builds this thread waited for
    uint32_t builds_ = 0;
    uint32_t inb_cut_ = 0, inb_nothing_ = 0, inb_diverged_ = 0;
    char interp_line_[1200] = "";
    uint32_t frames_ = 0, report_ = 0;
    u64 last_dl_tick_ = 0;                      // when the game submitted the last list
    uint32_t late_ = 0, late_logged_ = 0;
    float max_gap_ = 0, last_interp_ms_ = 0, last_replay_ms_ = 0;
    size_t last_draws_ = 0, last_verts_ = 0;
    uint32_t last_uploads_ = 0;
    uint32_t dl_count_ = 0;
    u64 window_start_ = 0, acc_gfx_ = 0, acc_replay_ = 0, acc_wait_ = 0, acc_end_ = 0, acc_depth_ = 0;
    float wait_ms_ = 0, end_ms_ = 0, depth_ms_ = 0;
    u64 prof_[5] = {};
    bool prof_on_ = false;   // PROFILE.TXT: time the replay's sections (a syscall per sample)
    u64 acc_tris_ = 0;       // the lists' deferred triangles (finish_triangles), for the read profile
    u64 acc_prefetch_ = 0;   // the lists' texture decoding (prefetch_textures), for the read profile
    u64 tick() const { return prof_on_ ? svcGetSystemTick() : 0; }
    uint32_t frames_at_profile_ = 0, frames_at_stereo_ = 0;
    C3D_Tex* last_tex_[2] = { nullptr, nullptr };
    uint32_t last_tex_param_[2] = { 0, 0 };
    uint32_t last_cc_[2] = { 0, 0 }, last_omh_ = 0, last_oml_ = 0, last_gm_ = 0;
    uint8_t last_prim_[4] = {}, last_env_[4] = {}, last_fog_[4] = {}, last_plf_ = 0, last_kind_ = 0;
    rt64_3ds::TevPlan last_plan_{};
    int plan_fallbacks_ = 0;            // the current plan's inexact constants
    static constexpr uint32_t kPlanCache = 256;
    rt64_3ds::TevPlan plan_cache_[kPlanCache]{};
    struct PlanKey { uint32_t w[8]; };
    PlanKey plan_keys_[kPlanCache]{};
    bool plan_valid_[kPlanCache]{};
    // What apply_state's depth/blend/alpha-test/scissor block last applied.
    struct MiscKey {
        uint32_t oml, omh_cycle;
        int16_t scissor[4];
        uint8_t kind, alpha_ref, rtt;
        bool operator==(const MiscKey& o) const {
            return oml == o.oml && omh_cycle == o.omh_cycle && kind == o.kind && alpha_ref == o.alpha_ref && rtt == o.rtt &&
                   scissor[0] == o.scissor[0] && scissor[1] == o.scissor[1] && scissor[2] == o.scissor[2] && scissor[3] == o.scissor[3];
        }
    };
    MiscKey last_misc_{};
    bool misc_valid_ = false;
    float last_us_ = -1.0f, last_vs_ = -1.0f;
    float frames_in_window_ = 1.0f;
    uint32_t frames_at_window_ = 0;
};

}   // namespace

std::unique_ptr<ultramodern::renderer::RendererContext>
rt64_3ds::create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    (void)window_handle; (void)developer_mode;
    return std::make_unique<C3dRenderContext>(rdram);
}


const rt64_3ds::Progress& rt64_3ds::progress() { return g_progress; }

void rt64_3ds::dump_progress() {
    fprintf(stderr, "rt64-3ds: progress: frame %u phase %u draw %u of %u\n", g_progress.frames, g_progress.phase, g_progress.draw_index, g_progress.draw_count);
    if (g_progress.phase == 3 && g_progress_draw != nullptr) C3dRenderContext::describe_draw(g_progress.draw_index, *g_progress_draw);
}

void rt64_3ds::set_debug_gfx_delay_ms(int ms) { g_debug_gfx_delay_ms = ms; }
void rt64_3ds::set_quitting() { g_quitting = true; }
void rt64_3ds::set_paused(bool paused) { g_paused = paused; }
void rt64_3ds::request_capture() { g_capture_request = true; }
void rt64_3ds::request_frame_sequence(int count) { g_seq_request = count; }
void rt64_3ds::request_capture_pair() { g_pair_request = true; }
int rt64_3ds::capture_state() { return capture::g_state; }
const char* rt64_3ds::capture_message(int* seq) { return capture::message(seq); }
bool rt64_3ds::capture_stop(int timeout_ms) {
    // Quitting: the writer stops between requests and closes its file, so
    // no FS request of ours is outstanding when the process tears FS down.
    capture::g_abort = true;
    const u64 until = svcGetSystemTick() + (u64)timeout_ms * (SYSCLOCK_ARM11 / 1000);
    while (capture::pending() > 0 && svcGetSystemTick() < until) svcSleepThread(10 * 1000000ll);
    const int left = capture::pending();
    if (left > 0) fprintf(stderr, "rt64-3ds: capture writer still busy at exit (%s, %lu of %lu bytes)\n", capture::g_writing,
                          (unsigned long)capture::g_done_bytes, (unsigned long)capture::g_total_bytes);
    return left == 0;
}
void rt64_3ds::request_screenshot() { g_shot_state = 0; g_shot_request = true; }
int rt64_3ds::screenshot_state() { return g_shot_state; }
const char* rt64_3ds::screenshot_name() { return g_shot_name; }

bool rt64_3ds::read_back_frame(uint8_t* rdram, uint32_t fb_addr, uint32_t fb_width, int x, int y, int w, int h) {
    LightLock_Lock(&g_readback_lock);
    const int ready = g_readback_ready;
    if (ready < 0) {
        LightLock_Unlock(&g_readback_lock);
        fprintf(stderr, "rt64-3ds: frame read-back asked for with no frame kept\n");
        return false;
    }
    const u16* src = g_readback[ready];
    const rt64_3ds::ScreenGeometry geo = rt64_3ds::screen_geometry();
    for (int j = 0; j < h; j++) {
        const int ny = y + j;                                      // N64 pixel
        const int sy = (int)geo.screen_y((float)ny + 0.5f);        // the screen pixel under its centre
        if (sy < 0 || sy >= 240) continue;
        for (int i = 0; i < w; i++) {
            const int sx = (int)geo.screen_x((float)(x + i) + 0.5f);
            if (sx < 0 || sx >= 400) continue;
            const u16 p = src[sx * 240 + 239 - sy] | 1;
            const uint32_t a = (fb_addr + ((uint32_t)ny * fb_width + (uint32_t)(x + i)) * 2) & 0xFFFFFE;
            *(u16*)(rdram + (a ^ 2)) = p;       // RDRAM halfwords sit at address ^ 2
        }
    }
    LightLock_Unlock(&g_readback_lock);
    fprintf(stderr, "rt64-3ds: frame read back into %06x (%d,%d %dx%d)\n", fb_addr & 0xFFFFFF, x, y, w, h);
    return true;
}
