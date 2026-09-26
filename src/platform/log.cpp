// stderr as whole lines to svcOutputDebugString (the emulator's log, 3dslink)
// and, flushed, to sdmc:/3ds/<Game>/log.txt so a crash or an emulator kill
// does not lose the tail. newlib hands an unbuffered stream to write() one
// fragment at a time, so lines are assembled here first.
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

void flush_line() {
    g_line[g_len] = '\n';
    g_line[g_len + 1] = '\0';
    svcOutputDebugString(g_line, g_len + 1);
    g_line[g_len] = '\0';
    recomp3ds::autotest_scan_line(g_line);
    g_line[g_len] = '\n';
    if (g_file != nullptr) {
        fwrite(g_line, 1, (size_t)g_len + 1, g_file);
        fflush(g_file);
    }
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
