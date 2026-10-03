// Equivalence fuzzer for the audio microcode interpreters: runs the working
// copy's naudio_hle.cpp / aspmain_hle.cpp next to an oracle copy of the same
// files (built into namespace recomp3ds_oracle; typically the last version
// checked against the recompiled microcode) on the same random work, and
// requires RDRAM and DMEM to come out byte for byte identical. A rewrite of
// an interpreter's arithmetic (audio_hle_simd.h) must pass this before it
// replaces code that was verified against the microcode.
//
//   audio_fuzz [cases] [aspMain capture files...]
//
// naudio (the n_aspMain family: DK64, Banjo-Kazooie): command lists built
// from scratch, with random DMEM (tables, codebooks, buffers) and random
// voice state, through both builds of the microcode (phase inversion on and
// off). aspMain (Goemon): each given capture (AUDIO_CAPTURE.TXT) as is, then
// mutated (pitches, gains, envelope flags, volumes, state buffers, sample
// buffers), at full quality and in low quality.
// Build with build_fuzz.sh.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "librecomp/rsp.hpp"

// The oracle's declarations, in their own namespace and header guards.
#define recomp3ds recomp3ds_oracle
#include ORACLE_NAUDIO_H
#include ORACLE_ASPMAIN_H
#undef recomp3ds
#undef RECOMP3DS_NAUDIO_HLE_H
#undef RECOMP3DS_ASPMAIN_HLE_H
#undef RECOMP3DS_AUDIO_HLE_DIFF_H
#include "naudio_hle.h"
#include "aspmain_hle.h"

namespace {

constexpr size_t kMem = 16u * 1024 * 1024;
constexpr uint32_t kUcodeData = 0x100000, kList = 0x200000, kWindow = 0x300000, kWindowSize = 0x80000;

inline void put_word(std::vector<uint8_t>& rd, uint32_t a, uint32_t v) { memcpy(&rd[a & 0xFFFFFC], &v, 4); }
inline uint32_t get_word(const std::vector<uint8_t>& rd, uint32_t a) { uint32_t v; memcpy(&v, &rd[a & 0xFFFFFC], 4); return v; }

std::mt19937 g_rng(12345);
uint32_t rnd() { return g_rng(); }
uint32_t rnd(uint32_t n) { return n ? rnd() % n : 0; }
bool chance(int percent) { return (int)rnd(100) < percent; }
void fill_random(std::vector<uint8_t>& rd, uint32_t a, uint32_t len) { for (uint32_t i = 0; i < len; i++) rd[(a + i) & 0xFFFFFF] = (uint8_t)rnd(); }
// Samples: mostly modest values (the buffers a voice leaves), sometimes full range.
void fill_samples(std::vector<uint8_t>& rd, uint32_t a, uint32_t len) {
    const int amp = chance(20) ? 32768 : (int)(256 + rnd(8000));
    for (uint32_t i = 0; i + 1 < len; i += 2) {
        const int16_t v = (int16_t)((int)rnd(2 * amp) - amp);
        rd[(a + i) & 0xFFFFFF] = (uint8_t)v; rd[(a + i + 1) & 0xFFFFFF] = (uint8_t)(v >> 8);
    }
}
uint32_t window_addr(uint32_t align) { return (kWindow + rnd(kWindowSize - 0x2000)) & ~(align - 1); }

void set_task(uint32_t ucode_data, uint32_t list, uint32_t cmds) {
    OSTask t{};
    t.t.type = M_AUDTASK;
    t.t.ucode_data = 0x80000000u | ucode_data;
    t.t.data_ptr = 0x80000000u | list;
    t.t.data_size = cmds * 8;
    memcpy(&dmem[0xFC0], &t, sizeof(OSTask));
}

struct Result { long cases = 0, failures = 0; };

bool compare(const char* what, long n, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const uint8_t* da, const uint8_t* db) {
    if (memcmp(da, db, 0x1000) != 0) {
        uint32_t i = 0; while (da[i] == db[i]) i++;
        printf("FAIL %s case %ld: DMEM differs from %03x (new %02x, oracle %02x)\n", what, n, i ^ 3, da[i], db[i]);
        return false;
    }
    if (memcmp(a.data(), b.data(), kMem) != 0) {
        size_t i = 0; while (a[i] == b[i]) i++;
        printf("FAIL %s case %ld: RDRAM differs at %06zx (new %02x, oracle %02x)\n", what, n, i ^ 3, a[i], b[i]);
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ naudio

void naudio_case(std::vector<uint8_t>& rd, uint32_t& cmds) {
    fill_random(rd, kUcodeData, 0xF80);
    // Codebook-like and sample-like content at the buffers the commands use.
    fill_random(rd, kWindow, kWindowSize);
    const uint32_t n = 1 + rnd(24);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t w0 = 0, w1 = 0;
        const uint32_t st = window_addr(8);
        if (chance(30)) fill_samples(rd, st, 0x60);
        switch (rnd(16)) {
            case 0: case 1:     // ADPCM
                w0 = (1u << 24) | st;
                w1 = (rnd(4) << 28) | ((chance(70) ? 32 * (1 + rnd(12)) : rnd(0x400)) << 16) | (rnd(16) << 12) |
                     (chance(70) ? 0x170 + 2 * rnd(8) : rnd(0x800) & ~1u);
                break;
            case 2: case 3: case 4:     // RESAMPLE: init, pitch, input offset, output select
                w0 = (5u << 24) | st;
                w1 = ((chance(25) ? 1u : 0u) << 30) | ((chance(80) ? 0x2000 + rnd(0xC000) : rnd(0x10000)) << 14) |
                     ((chance(70) ? 0x170 + 2 * rnd(32) : rnd(0x1000)) << 2) | rnd(4);
                break;
            case 5: case 6: case 7:     // ENVMIXER: init, right volume
                w0 = (3u << 24) | ((chance(30) ? 1u : 0u) << 16) | rnd(0x10000);
                w1 = st;
                if (chance(50)) fill_samples(rd, st, 0x60);
                break;
            case 8:     // SETVOL (any of its forms)
                w0 = (9u << 24) | (rnd(8) << 16) | rnd(0x10000);
                w1 = rnd();
                break;
            case 9: case 10:    // MIXER
                w0 = (12u << 24) | rnd(0x10000);
                w1 = chance(70) ? (((0x4E0u + 4 * rnd(64)) & 0xFFC) << 16) | ((0x4E0u + 4 * rnd(64)) & 0xFFC) : rnd();
                break;
            case 11:    // INTERLEAVE
                w0 = 13u << 24;
                break;
            case 12:    // LOADADPCM
                w0 = (11u << 24) | (8 * (1 + rnd(32)));
                w1 = window_addr(8);
                break;
            case 13:    // LOADBUFF / SAVEBUFF
                w0 = ((chance(50) ? 4u : 6u) << 24) | ((8 * rnd(64)) << 12) | (rnd(0x800) & ~7u);
                w1 = window_addr(8);
                break;
            case 14:    // CLEARBUFF / DMEMMOVE
                w0 = ((chance(50) ? 2u : 10u) << 24) | (rnd(0x800) & ~1u);
                w1 = ((rnd(0x800) & ~1u) << 16) | (16 * rnd(32));
                break;
            default:    // SETLOOP, 14, NOP
                w0 = (chance(50) ? 15u : 14u) << 24;
                w1 = window_addr(8);
                break;
        }
        put_word(rd, kList + 8 * i, w0);
        put_word(rd, kList + 8 * i + 4, w1);
    }
    cmds = n;
}

Result fuzz_naudio(long cases) {
    Result r;
    std::vector<uint8_t> a(kMem), b(kMem);
    fill_random(a, 0, kMem);
    for (long c = 0; c < cases; c++) {
        uint32_t cmds = 0;
        naudio_case(a, cmds);
        set_task(kUcodeData, kList, cmds);
        b = a;
        const bool dk = (c & 1) == 0;
        recomp3ds::naudio_hle_set_variant(dk ? recomp3ds::NaudioVariant::Dk64 : recomp3ds::NaudioVariant::BanjoKazooie);
        recomp3ds_oracle::naudio_hle_set_variant(dk ? recomp3ds_oracle::NaudioVariant::Dk64 : recomp3ds_oracle::NaudioVariant::BanjoKazooie);
        recomp3ds::naudio_hle_reset_dmem();
        recomp3ds_oracle::naudio_hle_reset_dmem();
        recomp3ds::naudio_hle_run(a.data(), 0);
        recomp3ds_oracle::naudio_hle_run(b.data(), 0);
        r.cases++;
        if (!compare(dk ? "naudio/dk64" : "naudio/banjo", c, a, b, recomp3ds::naudio_hle_dmem(), recomp3ds_oracle::naudio_hle_dmem())) {
            if (++r.failures >= 5) break;
            a = b;      // continue from the oracle's state
        }
    }
    return r;
}

// ----------------------------------------------------------------- aspMain

struct Capture { OSTask task; std::vector<uint8_t> rdram; };

bool load_capture(const char* path, Capture& cap) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return false; }
    cap.rdram.resize(kMem);
    const bool ok = fread(&cap.task, 1, sizeof(OSTask), f) == sizeof(OSTask) && fread(cap.rdram.data(), 1, kMem, f) == kMem;
    fclose(f);
    return ok;
}

// Mutations that keep the command list's structure: parameters, flags and
// the data the commands read.
void mutate_aspmain(std::vector<uint8_t>& rd, const OSTask& t) {
    const uint32_t list = t.t.data_ptr & 0xFFFFFF, n = t.t.data_size / 8;
    const int rate = 2 + (int)rnd(20);
    for (uint32_t i = 0; i < n; i++) {
        if (!chance(rate)) continue;
        uint32_t w0 = get_word(rd, list + 8 * i), w1 = get_word(rd, list + 8 * i + 4);
        const uint32_t op = (w0 >> 24) & 0x7F;
        switch (op) {
            case 5: // RESAMPLE: pitch, init
                w0 = (w0 & 0xFFFF0000u) | (chance(80) ? 0x2000 + rnd(0xC000) : rnd(0x10000));
                if (chance(20)) w0 ^= 1u << 16;
                break;
            case 3: // ENVMIXER: init, and its saved state when it continues
                if (chance(30)) w0 ^= 1u << 16;
                if ((w1 >> 24) == 0 && chance(50)) fill_random(rd, w1 & 0xFFFFF8, 0x50);
                break;
            case 1: // ADPCM: init, loop, and its saved state
                if (chance(30)) w0 ^= (rnd(3) + 1) << 16;
                if ((w1 >> 24) == 0 && chance(50)) fill_samples(rd, w1 & 0xFFFFF8, 32);
                break;
            case 12: // MIXER: gain
                w0 = (w0 & 0xFFFF0000u) | rnd(0x10000);
                break;
            case 9: // SETVOL: values
                w0 = (w0 & 0xFFFF0000u) | rnd(0x10000);
                if (chance(50)) w1 = rnd();
                break;
            case 8: // SETBUFF: shift a buffer or a count a little
                if (chance(50)) w1 = (w1 & 0xFFFF0000u) | ((w1 + 2 * (rnd(9) - 4)) & 0xFFFF);
                else w0 = (w0 & 0xFFFF0000u) | ((w0 + 2 * (rnd(9) - 4)) & 0xFFFF);
                break;
            case 4: // LOADBUF: new data where it loads from
                if ((w1 >> 24) == 0) fill_samples(rd, w1 & 0xFFFFF8, 0x100);
                break;
            default: break;
        }
        put_word(rd, list + 8 * i, w0);
        put_word(rd, list + 8 * i + 4, w1);
    }
}

Result fuzz_aspmain(long cases, const std::vector<Capture>& caps) {
    Result r;
    if (caps.empty()) return r;
    std::vector<uint8_t> a(kMem), b(kMem);
    for (long c = 0; c < cases; c++) {
        const Capture& cap = caps[c % caps.size()];
        const bool low = (c / (long)caps.size()) & 1;
        a = cap.rdram;
        if (c >= 2 * (long)caps.size()) mutate_aspmain(a, cap.task);      // the first rounds: the captures as they are
        memcpy(&dmem[0xFC0], &cap.task, sizeof(OSTask));
        b = a;
        recomp3ds::aspmain_hle_set_low_quality(low);
        recomp3ds_oracle::aspmain_hle_set_low_quality(low);
        recomp3ds::aspmain_hle_reset_dmem();
        recomp3ds_oracle::aspmain_hle_reset_dmem();
        recomp3ds::aspmain_hle_run(a.data(), cap.task.t.ucode);
        recomp3ds_oracle::aspmain_hle_run(b.data(), cap.task.t.ucode);
        r.cases++;
        if (!compare(low ? "aspmain/low" : "aspmain/full", c, a, b, recomp3ds::aspmain_hle_dmem(), recomp3ds_oracle::aspmain_hle_dmem())) {
            if (++r.failures >= 5) break;
        }
    }
    return r;
}

}   // namespace

int main(int argc, char** argv) {
    recomp::rsp::constants_init();
    const long cases = argc > 1 ? atol(argv[1]) : 2000;
    std::vector<Capture> caps;
    for (int i = 2; i < argc; i++) {
        Capture cap;
        if (load_capture(argv[i], cap)) caps.push_back(std::move(cap));
    }
    const Result n = fuzz_naudio(cases);
    printf("naudio: %ld cases (DK64 and Banjo builds), %ld failures\n", n.cases, n.failures);
    const Result a = fuzz_aspmain(cases, caps);
    printf("aspMain: %ld cases from %zu captures (full and low quality), %ld failures\n", a.cases, caps.size(), a.failures);
    return (n.failures || a.failures) ? 1 : 0;
}
