#include <3ds.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "rt64_3ds_dl.h"

// OSTask as the runtime lays it out: data_ptr is the 13th word.
struct TaskWords { uint32_t w[16]; };

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    svcOutputDebugString(buf, strlen(buf));
    if (FILE* f = fopen("sdmc:/3ds/dl_bench/result.txt", "a")) { fputs(buf, f); fclose(f); }
}

int main() {
    say("dl_bench: start\n");
    FILE* f = fopen("sdmc:/3ds/DK64/gfx_task.bin", "rb");
    if (!f) { say("dl_bench: no sdmc:/3ds/DK64/gfx_task.bin\n"); return 1; }
    TaskWords t;
    fread(&t, 1, sizeof(t), f);
    uint8_t* rdram = (uint8_t*)malloc(16u * 1024 * 1024);
    fread(rdram, 1, 16u * 1024 * 1024, f);
    fclose(f);
    const uint32_t data_ptr = t.w[12];
    rt64_3ds::Interpreter interp(rdram);
    rt64_3ds::FrameRecord frame;
    const int runs = 20;
    for (int pass = 0; pass < 3; pass++) {
        u64 t0 = svcGetSystemTick();
        for (int i = 0; i < runs; i++) { frame.clear(); interp.run(data_ptr, frame); }
        u64 t1 = svcGetSystemTick();
        say("dl_bench: %.3f ms per run (%zu draws, %u verts)\n",
                (double)(t1 - t0) * 1000.0 / SYSCLOCK_ARM11 / runs, frame.draws.size(), frame.verts.size());
    }
    say("dl_bench: done\n");
    return 0;
}
