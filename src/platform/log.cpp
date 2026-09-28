// stderr as whole lines to svcOutputDebugString (the emulator's log, 3dslink)
// and to sdmc:/3ds/<Game>/log.txt. newlib hands an unbuffered stream to
// write() one fragment at a time, so lines are assembled here first.
//
// The SD writes happen on a writer thread fed through a ring
// buffer: flushing each line from the thread that logged it stalled the
// renderer 70-100 ms whenever it wrote its periodic report. Lines reach the
// card within ~50 ms, so a crash loses at most that tail; before the writer
// starts and on exit (log_flush) the writes are synchronous.
#include <3ds.h>
#include <cstdio>
#include <cstring>
#include <sys/iosupport.h>

#include "recomp3ds_internal.h"
#include "recomp3ds.h"

namespace {

char g_line[512];
int g_len = 0;
FILE* g_file = nullptr;
LightLock g_lock;

// The ring the writer drains (guarded by g_ring_lock, never held over I/O).
constexpr size_t kRing = 64 * 1024;
char g_ring[kRing];
size_t g_head = 0, g_tail = 0;       // write at head, read from tail
uint32_t g_dropped = 0;
LightLock g_ring_lock;
Thread g_writer = nullptr;
volatile bool g_writer_run = false;

void write_file(const char* p, size_t n) {
    if (g_file != nullptr) {
        fwrite(p, 1, n, g_file);
        fflush(g_file);
    }
}

// Moves what the ring holds to the card; returns whether anything was written.
bool drain_ring() {
    static char chunk[8192];
    bool any = false;
    for (;;) {
        LightLock_Lock(&g_ring_lock);
        size_t n = 0;
        while (g_tail != g_head && n < sizeof(chunk)) {
            chunk[n++] = g_ring[g_tail];
            g_tail = (g_tail + 1) % kRing;
        }
        uint32_t dropped = g_dropped;
        g_dropped = 0;
        LightLock_Unlock(&g_ring_lock);
        if (dropped != 0) {
            char note[64];
            int k = snprintf(note, sizeof(note), "[log: %lu bytes dropped]\n", (unsigned long)dropped);
            write_file(note, (size_t)k);
        }
        if (n == 0) return any;
        write_file(chunk, n);
        any = true;
    }
}

void writer_thread(void*) {
    while (g_writer_run) {
        drain_ring();
        svcSleepThread(50 * 1000000ll);
    }
    drain_ring();
}

void queue_line(const char* p, size_t n) {
    if (g_writer == nullptr) {       // before the writer starts: straight out
        write_file(p, n);
        return;
    }
    LightLock_Lock(&g_ring_lock);
    const size_t used = (g_head + kRing - g_tail) % kRing;
    if (used + n >= kRing) {
        g_dropped += (uint32_t)n;
    }
    else {
        for (size_t i = 0; i < n; i++) {
            g_ring[g_head] = p[i];
            g_head = (g_head + 1) % kRing;
        }
    }
    LightLock_Unlock(&g_ring_lock);
}

void flush_line() {
    g_line[g_len] = '\n';
    g_line[g_len + 1] = '\0';
    svcOutputDebugString(g_line, g_len + 1);
    g_line[g_len] = '\0';
    recomp3ds::autotest_scan_line(g_line);
    g_line[g_len] = '\n';
    queue_line(g_line, (size_t)g_len + 1);
    g_len = 0;
}

ssize_t stderr_write(struct _reent* r, void* fd, const char* ptr, size_t len) {
    (void)r; (void)fd;
    LightLock_Lock(&g_lock);
    for (size_t i = 0; i < len; i++) {
        char c = ptr[i];
        if (c == '\n') {
            flush_line();
        }
        else {
            g_line[g_len++] = c;
            if (g_len >= (int)sizeof(g_line) - 2) {
                flush_line();
            }
        }
    }
    LightLock_Unlock(&g_lock);
    return (ssize_t)len;
}

const devoptab_t g_errtab = {
    .name = "recomp3ds_stderr",
    .structSize = 0,
    .write_r = stderr_write,
};

}   // namespace

void recomp3ds::log_init(const char* base_path) {
    LightLock_Init(&g_lock);
    LightLock_Init(&g_ring_lock);
    char path[192];
    snprintf(path, sizeof(path), "%s/log.txt", base_path);
    g_file = fopen(path, "w");
    // 3dslink owns stderr through its own socket devoptab; leave that alone.
    if (__3dslink_host.s_addr == 0) {
        devoptab_list[STD_ERR] = &g_errtab;
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
}

void recomp3ds::log_line(const char* s) {
    fprintf(stderr, "%s\n", s);
}

// From here on lines go through the writer thread. It sits just above the
// game threads: it needs almost no CPU (it mostly waits on the card), and
// below them it starved whenever the game kept core 0 busy.
void recomp3ds::log_start_writer() {
    if (g_writer != nullptr || g_file == nullptr) return;
    g_writer_run = true;
    g_writer = threadCreate(writer_thread, nullptr, 16 * 1024, 0x2F, 0, false);
}

// Everything logged so far onto the card, and synchronous from then on
// (exit paths).
void recomp3ds::log_flush() {
    if (g_writer != nullptr) {
        g_writer_run = false;
        threadJoin(g_writer, 1000000000ull);
        threadFree(g_writer);
        g_writer = nullptr;
    }
    drain_ring();
}
