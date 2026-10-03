// Host-side test for the audio microcode interpreters: replays a captured
// audio task (AUDIO_CAPTURE.TXT, see audio_hle_diff.h) through the
// recompiled microcode and through the interpreter, and compares what they
// write. Build with tools/naudio_test/build.sh <game repo>: HLE=naudio (the
// n_aspMain family, naudio_hle.cpp) by default, HLE=aspmain for libaudio's
// ABI 1 aspMain (aspmain_hle.cpp).
//
//   naudio_test task.bin                whole task, both ways; RDRAM compared
//                                       (aspmain: DMEM too)
//   naudio_test task.bin --lockstep [N] the interpreter's own per-command
//                                       differential mode; N mismatches in full
//   naudio_test task.bin --ref-crc      checksums of what the reference wrote,
//                                       to compare two builds of it (SIMD, SISD)
//   naudio_test task.bin --bench N      time N interpreter runs (BENCH_REF=1:
//                                       the recompiled microcode too)
//   naudio_test task.bin --synth        (naudio) a DK64 envelope-init case
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "librecomp/rsp.hpp"
#include "naudio_hle.h"
#include "aspmain_hle.h"

#ifndef HLE_FAMILY
#define HLE_FAMILY 0
#endif
#ifndef UCODE_FUNC
#define UCODE_FUNC n_aspMain
#endif
RspExitReason UCODE_FUNC(uint8_t* rdram, uint32_t ucode_addr);

namespace {

constexpr bool kAspmain = HLE_FAMILY == 1;

inline uint8_t rb(const uint8_t* rdram, uint32_t p) { return rdram[(p & 0xFFFFFF) ^ 3]; }
inline uint32_t rw(const uint8_t* rdram, uint32_t p) {
    return ((uint32_t)rb(rdram, p) << 24) | ((uint32_t)rb(rdram, p + 1) << 16) | ((uint32_t)rb(rdram, p + 2) << 8) | rb(rdram, p + 3);
}

struct Range { uint32_t addr, len; uint32_t cmd; uint32_t op; };

// naudio: the ranges its commands name (its addresses are plain).
void naudio_ranges(const uint8_t* rdram, const OSTask& task, std::vector<Range>& out) {
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

// Any family: the RDRAM a run changed, as runs of whole words merged across
// gaps of less than 16 bytes (RDRAM is stored word-swapped, so the host
// offsets of a word are its N64 addresses).
void changed_ranges(const std::vector<uint8_t>& base, const std::vector<uint8_t>& after, std::vector<Range>& out) {
    const size_t n = base.size();
    size_t i = 0;
    while (i < n) {
        if (base[i] == after[i]) { i++; continue; }
        size_t start = i & ~(size_t)3, last = i;
        for (bool more = true; more;) {
            more = false;
            for (size_t j = last + 1; j < n && j < last + 16; j++) if (base[j] != after[j]) { last = j; more = true; break; }
        }
        size_t end = (last | 3) + 1;
        out.push_back({ (uint32_t)start, (uint32_t)(end - start), 0, 0 });
        i = end;
    }
}

void prepare_dmem(uint8_t* rdram, const OSTask& task) {
    // As librecomp's run_task, from a cleared DMEM so that both runs see the
    // same leftovers (the interpreter's DMEM is cleared the same way).
    memset(dmem, 0, 0x1000);
    memcpy(&dmem[0xFC0], &task, sizeof(OSTask));
    dma_rdram_to_dmem(rdram, 0, task.t.ucode_data, 0xF80 - 1);
}

void run_reference(uint8_t* rdram, const OSTask& task) {
    prepare_dmem(rdram, task);
    RspExitReason r = UCODE_FUNC(rdram, task.t.ucode);
    if (r != RspExitReason::Broke) printf("reference exited with %d\n", (int)r);
}

void run_hle(uint8_t* rdram, const OSTask& task) {
    prepare_dmem(rdram, task);
    if constexpr (kAspmain) {
        // HLE_LOW=1: the low-quality path (not bit-exact: compare its audio, not its bytes).
        recomp3ds::aspmain_hle_set_low_quality(getenv("HLE_LOW") != nullptr);
        recomp3ds::aspmain_hle_set_no_reverb(getenv("HLE_NOFX") != nullptr);
        recomp3ds::aspmain_hle_reset_dmem();
        recomp3ds::aspmain_hle_run(rdram, task.t.ucode);
    }
    else {
        recomp3ds::naudio_hle_run(rdram, task.t.ucode);
    }
}

const recomp3ds::AudioHleStats& hle_stats() {
    if constexpr (kAspmain) return recomp3ds::aspmain_hle_stats();
    return recomp3ds::naudio_hle_stats();
}

}   // namespace

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: naudio_test audio_task.bin [--lockstep [N] | --ref-crc | --bench N | --synth]\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    OSTask task;
    if (fread(&task, 1, sizeof(OSTask), f) != sizeof(OSTask)) { fprintf(stderr, "short file\n"); return 2; }
    const size_t mem = 16u * 1024 * 1024;
    std::vector<uint8_t> base(mem);
    if (fread(base.data(), 1, mem, f) != mem) { fprintf(stderr, "short rdram\n"); return 2; }
    fclose(f);
    recomp::rsp::constants_init();
    // NAUDIO_VARIANT=bk: Banjo-Kazooie's build of the microcode (DK64's by default).
    if (const char* v = getenv("NAUDIO_VARIANT")) if (strcmp(v, "bk") == 0) recomp3ds::naudio_hle_set_variant(recomp3ds::NaudioVariant::BanjoKazooie);

    printf("task: data_ptr %08x size %u (%u cmds) ucode_data %08x\n", task.t.data_ptr, task.t.data_size, task.t.data_size / 8, task.t.ucode_data);
    if (argc > 3 && strcmp(argv[2], "--bench") == 0) {
        // --bench N: time N interpreter runs (fresh RDRAM each time with
        // BENCH_RESET) and the command mix, for profiling.
        int n = atoi(argv[3]);
        std::vector<uint8_t> work(mem);
        auto t0 = std::chrono::steady_clock::now();
        memcpy(work.data(), base.data(), mem);
        for (int i = 0; i < n; i++) {
            if (getenv("BENCH_RESET")) memcpy(work.data(), base.data(), mem);
            run_hle(work.data(), task);
        }
        auto t1 = std::chrono::steady_clock::now();
        auto t2 = std::chrono::steady_clock::now();
        if (getenv("BENCH_RESET")) for (int i = 0; i < n; i++) memcpy(work.data(), base.data(), mem);
        auto t3 = std::chrono::steady_clock::now();
        double ms = (std::chrono::duration<double, std::milli>(t1 - t0).count() - std::chrono::duration<double, std::milli>(t3 - t2).count()) / n;
        uint32_t hist[128] = {};
        uint32_t list = task.t.data_ptr & 0xFFFFFF;
        for (uint32_t i = 0; i < task.t.data_size / 8; i++) hist[(rw(base.data(), list + i * 8) >> 24) & 127]++;
        printf("%.3f ms per task on this host; commands:", ms);
        for (int op = 0; op < 128; op++) if (hist[op]) printf(" %d:%u", op, hist[op]);
        printf("\n");
        if (kAspmain) {
            // The interpreter's own per-opcode clock (AudioHleStats::op_ticks,
            // nanoseconds on a host): the share of each command.
            static const char* const kOps[16] = { "nop", "adpcm", "clear", "envmix", "loadbuf", "resamp", "savebuf", "segment",
                                                  "setbuff", "setvol", "dmemmv", "loadadp", "mixer", "interlv", "polef", "setloop" };
            const recomp3ds::AudioHleStats& hs = recomp3ds::aspmain_hle_stats();
            uint64_t total = 0;
            for (int op = 0; op < 16; op++) total += hs.op_ticks[op];
            printf("time by command:");
            for (int op = 0; op < 16; op++) if (hs.op_ticks[op] * 100ull >= total) printf(" %s %.1f%% (%.1f us/task, %u/task)", kOps[op], 100.0 * hs.op_ticks[op] / (double)(total ? total : 1), hs.op_ticks[op] / 1000.0 / n, hist[op]);
            printf("\n");
        }
        if (getenv("BENCH_REF")) {
            // BENCH_REF=1: the recompiled microcode, for the ratio.
            auto r0 = std::chrono::steady_clock::now();
            for (int i = 0; i < n; i++) run_reference(work.data(), task);
            auto r1 = std::chrono::steady_clock::now();
            printf("%.3f ms per task for the recompiled microcode\n", std::chrono::duration<double, std::milli>(r1 - r0).count() / n);
        }
        return 0;
    }

    // --synth: the envelope-mixer init case a 3DS lockstep run flagged (state
    // values from its dump), run through the reference and the interpreter.
    if (!kAspmain && argc > 2 && strcmp(argv[2], "--synth") == 0) {
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
        const int verbose = argc > 3 ? atoi(argv[3]) : 0;   // --lockstep N: dump N mismatches in full
        if constexpr (kAspmain) {
            recomp3ds::aspmain_hle_set_reference(UCODE_FUNC);
            recomp3ds::aspmain_hle_set_diff_verbose(verbose);
        }
        else {
            recomp3ds::naudio_hle_set_reference(UCODE_FUNC);
            recomp3ds::naudio_hle_set_diff_verbose(verbose);
        }
        std::vector<uint8_t> work = base;
        run_hle(work.data(), task);
        const recomp3ds::AudioHleStats& st = hle_stats();
        printf("lockstep: %u mismatching commands of %u; %u unknown, %u inexact\n", st.diff_mismatches, task.t.data_size / 8,
               st.unknown_opcodes, st.inexact);
        return st.diff_mismatches ? 1 : 0;
    }

    std::vector<uint8_t> ref = base, mine = base;
    run_reference(ref.data(), task);
    static uint8_t ref_dmem[0x1000];
    memcpy(ref_dmem, dmem, 0x1000);
    run_hle(mine.data(), task);
    if (const char* dump = getenv("DUMP_RDRAM")) {
        // What the interpreter left in RDRAM, for comparing its audio offline.
        if (FILE* o = fopen(dump, "wb")) { fwrite(mine.data(), 1, mine.size(), o); fclose(o); }
    }

    std::vector<Range> ranges;
    if constexpr (kAspmain) changed_ranges(base, ref, ranges);
    else naudio_ranges(base.data(), task, ranges);

    // --ref-crc: print a checksum per written range of the reference run, to
    // compare two builds of the reference (SIMD vs scalar) with each other.
    if (argc > 2 && strcmp(argv[2], "--ref-crc") == 0) {
        for (const Range& r : ranges) {
            uint32_t h = 2166136261u;
            for (uint32_t i = 0; i < r.len; i++) { h ^= rb(ref.data(), r.addr + i); h *= 16777619u; }
            printf("cmd %u op %u %06x %u %08x\n", r.cmd, r.op, r.addr, r.len, h);
        }
        if constexpr (kAspmain) {
            uint32_t h = 2166136261u;
            for (uint32_t i = 0; i < 0x1000; i++) { h ^= ref_dmem[i ^ 3]; h *= 16777619u; }
            printf("dmem %08x\n", h);
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
    // Everything else: the whole of RDRAM, and for aspmain the final DMEM.
    size_t rdram_diff = 0, first_diff = 0;
    for (size_t i = 0; i < mem; i++) if (ref[i] != mine[i]) { if (!rdram_diff) first_diff = i; rdram_diff++; }
    int dmem_diff = 0;
    if constexpr (kAspmain) {
        const uint8_t* hd = recomp3ds::aspmain_hle_dmem();
        for (uint32_t a = 0; a < 0x1000; a += 2) {
            uint16_t x = (uint16_t)((ref_dmem[a ^ 3] << 8) | ref_dmem[(a + 1) ^ 3]), y = (uint16_t)((hd[a ^ 3] << 8) | hd[(a + 1) ^ 3]);
            if (x != y) {
                if (dmem_diff < 12) printf("dmem %03x: ref %04x hle %04x\n", a, x, y);
                dmem_diff++;
            }
        }
    }
    const recomp3ds::AudioHleStats& st = hle_stats();
    printf("%zu ranges, %d differ; rdram bytes differing %zu", ranges.size(), bad, rdram_diff);
    if (rdram_diff) printf(" from %06zx", first_diff ^ 3);
    if (kAspmain) printf("; dmem halfwords differing %d", dmem_diff);
    printf("; hle ran %u commands, %u unknown, %u inexact\n", st.commands, st.unknown_opcodes, st.inexact);
    return (bad || rdram_diff || dmem_diff) ? 1 : 0;
}
