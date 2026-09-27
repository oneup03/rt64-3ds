// Host-side test for the naudio HLE: replays a captured audio task (see
// AUDIO_CAPTURE.TXT in naudio_hle.cpp) through the recompiled microcode
// and through the interpreter, and compares every RDRAM range the task
// writes. Build with tools/naudio_test/build.sh <game repo>.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "librecomp/rsp.hpp"
#include "naudio_hle.h"

RspExitReason n_aspMain(uint8_t* rdram, uint32_t ucode_addr);

namespace {

inline uint8_t rb(const uint8_t* rdram, uint32_t p) { return rdram[(p & 0xFFFFFF) ^ 3]; }
inline uint32_t rw(const uint8_t* rdram, uint32_t p) {
    return ((uint32_t)rb(rdram, p) << 24) | ((uint32_t)rb(rdram, p + 1) << 16) | ((uint32_t)rb(rdram, p + 2) << 8) | rb(rdram, p + 3);
}

struct Range { uint32_t addr, len; uint32_t cmd; uint32_t op; };

void ranges_of(const uint8_t* rdram, const OSTask& task, std::vector<Range>& out) {
    uint32_t list = task.t.data_ptr & 0xFFFFFF;
    for (uint32_t i = 0; i < task.t.data_size / 8; i++) {
        uint32_t w0 = rw(rdram, list + i * 8), w1 = rw(rdram, list + i * 8 + 4);
        switch (w0 >> 24) {
            case 1: out.push_back({ w0 & 0xFFFFF8, 32, i, 1 }); break;
            case 3: out.push_back({ w1 & 0xFFFFF8, 0x50, i, 3 }); break;
            case 5: out.push_back({ w0 & 0xFFFFF8, 16, i, 5 }); break;
            case 6: if ((w0 >> 12) & 0xFFF) out.push_back({ w1 & 0xFFFFF8, (w0 >> 12) & 0xFFF, i, 6 }); break;
            default: break;
        }
    }
}

void run_reference(uint8_t* rdram, const OSTask& task) {
    memcpy(&dmem[0xFC0], &task, sizeof(OSTask));
    dma_rdram_to_dmem(rdram, 0, task.t.ucode_data, 0xF80 - 1);
    n_aspMain(rdram, task.t.ucode);
}

void run_hle(uint8_t* rdram, const OSTask& task) {
    memcpy(&dmem[0xFC0], &task, sizeof(OSTask));
    dma_rdram_to_dmem(rdram, 0, task.t.ucode_data, 0xF80 - 1);
    recomp3ds::naudio_hle_run(rdram, task.t.ucode);
}

}   // namespace

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: naudio_test audio_task.bin\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    OSTask task;
    if (fread(&task, 1, sizeof(OSTask), f) != sizeof(OSTask)) { fprintf(stderr, "short file\n"); return 2; }
    const size_t mem = 16u * 1024 * 1024;
    std::vector<uint8_t> base(mem);
    if (fread(base.data(), 1, mem, f) != mem) { fprintf(stderr, "short rdram\n"); return 2; }
    fclose(f);
    recomp::rsp::constants_init();

    printf("task: data_ptr %08x size %u (%u cmds) ucode_data %08x\n", task.t.data_ptr, task.t.data_size, task.t.data_size / 8, task.t.ucode_data);
    std::vector<Range> ranges;
    ranges_of(base.data(), task, ranges);

    // --synth: the envelope-mixer init case a 3DS lockstep run flagged (state
    // values from its dump), run through the reference and the interpreter.
    if (argc > 2 && strcmp(argv[2], "--synth") == 0) {
        const uint32_t list = 0x00F00000, state = 0x00F01000;
        auto put = [&](uint8_t* rd, uint32_t p, uint32_t v) {
            rd[(p) ^ 3] = v >> 24; rd[(p + 1) ^ 3] = v >> 16; rd[(p + 2) ^ 3] = v >> 8; rd[(p + 3) ^ 3] = v;
        };
        const uint32_t cmds[] = {
            0x02000000, 0x00000170,     // clear input
            0x020004E0, 0x000002E0,     // clear main
            0x020007C0, 0x000002E0,     // clear aux
            0x09000000, 0xFE8837A5,     // RATE: ltgt 0, lratm fe88, lratl 37a5
            0x09040000, 0xFE83858F,     // RIGHT|VOL: rtgt 0, rratm fe83, rratl 858f
            0x09068079, 0x684A4A34,     // LEFT|VOL: cvolL 8079, dry 684a, wet 4a34
            0x03012230, state,          // ENVMIXER init, cvolR 2230
        };
        for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) put(base.data(), list + 4 * i, cmds[i]);
        OSTask t = task;
        t.t.data_ptr = list;
        t.t.data_size = sizeof(cmds);
        std::vector<uint8_t> a = base, b = base;
        run_reference(a.data(), t);
        run_hle(b.data(), t);
        printf("ref  state:"); for (uint32_t i = 0; i < 0x50; i += 2) printf(" %02x%02x", rb(a.data(), state + i), rb(a.data(), state + i + 1)); printf("\n");
        printf("hle  state:"); for (uint32_t i = 0; i < 0x50; i += 2) printf(" %02x%02x", rb(b.data(), state + i), rb(b.data(), state + i + 1)); printf("\n");
        return 0;
    }

    // --lockstep: the interpreter's own per-command differential mode, with
    // the recompiled microcode as reference (as on the 3DS with AUDIO_DIFF.TXT).
    if (argc > 2 && strcmp(argv[2], "--lockstep") == 0) {
        recomp3ds::naudio_hle_set_reference(n_aspMain);
        std::vector<uint8_t> work = base;
        run_hle(work.data(), task);
        const recomp3ds::NaudioHleStats& st = recomp3ds::naudio_hle_stats();
        printf("lockstep: %u mismatching commands\n", st.diff_mismatches);
        return st.diff_mismatches ? 1 : 0;
    }

    std::vector<uint8_t> ref = base, mine = base;
    run_reference(ref.data(), task);
    run_hle(mine.data(), task);

    // --ref-crc: print a checksum per written range of the reference run, to
    // compare two builds of the reference (SIMD vs scalar) with each other.
    if (argc > 2 && strcmp(argv[2], "--ref-crc") == 0) {
        for (const Range& r : ranges) {
            uint32_t h = 2166136261u;
            for (uint32_t i = 0; i < r.len; i++) { h ^= rb(ref.data(), r.addr + i); h *= 16777619u; }
            printf("cmd %u op %u %06x %u %08x\n", r.cmd, r.op, r.addr, r.len, h);
        }
    }

    int bad = 0;
    for (const Range& r : ranges) {
        int maxd = 0; uint32_t count = 0, first = 0;
        for (uint32_t i = 0; i + 1 < r.len; i += 2) {
            int16_t a = (int16_t)((rb(ref.data(), r.addr + i) << 8) | rb(ref.data(), r.addr + i + 1));
            int16_t b = (int16_t)((rb(mine.data(), r.addr + i) << 8) | rb(mine.data(), r.addr + i + 1));
            if (a != b) { int d = abs(a - b); if (d > maxd) maxd = d; if (!count) first = i; count++; }
        }
        if (count) {
            bad++;
            if (bad <= 20) printf("cmd %u op %u range %06x len %u: %u halfwords differ, max %d, first +%u\n", r.cmd, r.op, r.addr, r.len, count, maxd, first);
        }
    }
    const recomp3ds::NaudioHleStats& st = recomp3ds::naudio_hle_stats();
    printf("%zu ranges, %d differ; hle ran %u commands, %u unknown\n", ranges.size(), bad, st.commands, st.unknown_opcodes);
    return bad ? 1 : 0;
}
