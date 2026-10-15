// Differential testing and task capture for the audio microcode
// interpreters; see audio_hle_diff.h.
#include "audio_hle_diff.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "librecomp/addresses.hpp"

namespace {

// RDRAM's size, which a game may set below 16 MB (RT64_3DS_MEM_SIZE_MB):
// addresses wrap at 16 MB as the RSP's DMA does, then at its end, so a
// corrupted one never reaches past it.
constexpr uint32_t kRdram = (uint32_t)recomp::mem_size;
inline uint32_t ra(uint32_t p) { p &= 0xFFFFFF; return p < kRdram ? p : p % kRdram; }

inline uint8_t rb(const uint8_t* rdram, uint32_t p) { return rdram[ra(p) ^ 3]; }
inline void rbw(uint8_t* rdram, uint32_t p, uint8_t v) { rdram[ra(p) ^ 3] = v; }
inline uint32_t rw(const uint8_t* rdram, uint32_t p) {
    return ((uint32_t)rb(rdram, p) << 24) | ((uint32_t)rb(rdram, p + 1) << 16) | ((uint32_t)rb(rdram, p + 2) << 8) | rb(rdram, p + 3);
}
inline uint16_t mh(const uint8_t* m, uint32_t a) { return (uint16_t)((m[(a & 0xFFF) ^ 3] << 8) | m[((a + 1) & 0xFFF) ^ 3]); }

// Without a family dump: the first differing halfwords.
void generic_dump(const recomp3ds::AudioHleFamily& f, uint32_t i, uint32_t w0, uint32_t w1,
                  const uint8_t* before, const uint8_t* mine, const uint8_t* ref) {
    fprintf(stderr, "  cmd %u %08x %08x, DMEM halfwords that differ (mine/ref/before):", (unsigned)i, (unsigned)w0, (unsigned)w1);
    int shown = 0, total = 0;
    for (uint32_t a = 0; a < 0x1000; a += 2) {
        if (f.dmem_ignored != nullptr && f.dmem_ignored(a)) continue;
        if (mh(mine, a) == mh(ref, a)) continue;
        total++;
        if (shown < 24) { fprintf(stderr, " %03x:%04x/%04x/%04x", (unsigned)a, mh(mine, a), mh(ref, a), mh(before, a)); shown++; }
    }
    fprintf(stderr, " (%d in all)\n", total);
}

}   // namespace

void recomp3ds::audio_hle_dma_read(uint8_t* dm, const uint8_t* rdram, uint32_t dmem, uint32_t dram, uint32_t len) {
    dram &= 0xFFFFF8;
    // The copy in one piece only when it stays inside DMEM and RDRAM; byte by
    // byte otherwise, wrapping at 4 KB and RDRAM's end (a corrupted address
    // near the top of RDRAM must not read or write past it).
    if ((dmem & 3) == 0 && dmem + len <= 0x1000 && dram + len <= kRdram) {
        uint32_t whole = len & ~3u;
        memcpy(&dm[dmem], &rdram[dram], whole);
        for (uint32_t i = whole; i < len; i++) dm[(dmem + i) ^ 3] = rb(rdram, dram + i);
        return;
    }
    for (uint32_t i = 0; i < len; i++) dm[((dmem + i) & 0xFFF) ^ 3] = rb(rdram, dram + i);
}

void recomp3ds::audio_hle_dma_write(const uint8_t* dm, uint8_t* rdram, uint32_t dmem, uint32_t dram, uint32_t len) {
    dram &= 0xFFFFF8;
    if ((dmem & 3) == 0 && dmem + len <= 0x1000 && dram + len <= kRdram) {
        uint32_t whole = len & ~3u;
        memcpy(&rdram[dram], &dm[dmem], whole);
        for (uint32_t i = whole; i < len; i++) rbw(rdram, dram + i, dm[(dmem + i) ^ 3]);
        return;
    }
    for (uint32_t i = 0; i < len; i++) rbw(rdram, dram + i, dm[((dmem + i) & 0xFFF) ^ 3]);
}

void recomp3ds::audio_hle_lockstep(AudioHleFamily& f, uint8_t* rdram, const OSTask& task, uint32_t ucode_addr,
                                   RspUcodeFunc* reference, int* verbose) {
    uint8_t* const dm = f.dm;
    AudioHleStats& stats = *f.stats;
    // The task's DMEM as librecomp prepares it: tables from the ucode data,
    // the task at 0xFC0.
    audio_hle_dma_read(dm, rdram, 0, task.t.ucode_data, 0xF80);
    memcpy(&dm[0xFC0], &task, sizeof(OSTask));
    const uint32_t list = ra(task.t.data_ptr);
    const uint32_t n = task.t.data_size / 8;
    static uint8_t ref_dm[0x1000], pre_dm[0x1000];
    static std::vector<uint8_t> before, ref;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t w0 = rw(rdram, list + i * 8), w1 = rw(rdram, list + i * 8 + 4);
        const uint32_t op = w0 >> 24;
        uint32_t raddr = 0, rlen = 0;
        const bool has_range = f.command_range(w0, w1, &raddr, &rlen) && rlen != 0;
        if (has_range && before.size() < rlen + 2) { before.resize(rlen + 2); ref.resize(rlen + 2); }

        // The reference on a task of this command alone, from the interpreter's DMEM.
        if (*verbose > 0) memcpy(pre_dm, dm, 0x1000);
        memcpy(dmem, dm, 0x1000);
        const uint32_t one_ptr = task.t.data_ptr + i * 8, one_size = 8;
        memcpy(&dmem[0xFC0 + 0x30], &one_ptr, 4);
        memcpy(&dmem[0xFC0 + 0x34], &one_size, 4);
        memcpy(&dm[0xFC0 + 0x30], &one_ptr, 4);
        memcpy(&dm[0xFC0 + 0x34], &one_size, 4);
        if (has_range) for (uint32_t k = 0; k < rlen; k++) before[k] = rb(rdram, raddr + k);
        reference(rdram, ucode_addr);
        memcpy(ref_dm, dmem, 0x1000);
        if (has_range) {
            for (uint32_t k = 0; k < rlen; k++) { ref[k] = rb(rdram, raddr + k); rbw(rdram, raddr + k, before[k]); }
        }

        f.run_command(rdram, w0, w1);

        int bad_dmem = -1, bad_rdram = -1;
        for (uint32_t a = 0; a < 0x1000; a++) {
            if (f.dmem_ignored != nullptr && f.dmem_ignored(a)) continue;
            if (dm[a ^ 3] != ref_dm[a ^ 3]) { bad_dmem = (int)a; break; }
        }
        if (has_range) {
            for (uint32_t k = 0; k < rlen; k++) {
                if (rb(rdram, raddr + k) != ref[k]) { bad_rdram = (int)k; break; }
            }
        }
        if ((bad_dmem >= 0 || bad_rdram >= 0) && op < 16) {
            stats.diff_mismatches++;
            if (f.reports_by_op[op] < 3) {
                f.reports_by_op[op]++;
                if (bad_dmem >= 0) {
                    uint32_t a = (uint32_t)bad_dmem & ~1u;
                    fprintf(stderr, "%s: task %u cmd %u op %u %08x %08x: DMEM differs at %03x (mine %04x ref %04x)\n",
                            f.log_name, (unsigned)(stats.diff_tasks + 1), (unsigned)i, (unsigned)op, (unsigned)w0, (unsigned)w1,
                            (unsigned)a, mh(dm, a), mh(ref_dm, a));
                }
                if (bad_rdram >= 0) {
                    uint32_t k = (uint32_t)bad_rdram & ~1u;
                    fprintf(stderr, "%s: task %u cmd %u op %u %08x %08x: RDRAM differs at %06x+%u (mine %02x%02x ref %02x%02x)\n",
                            f.log_name, (unsigned)(stats.diff_tasks + 1), (unsigned)i, (unsigned)op, (unsigned)w0, (unsigned)w1,
                            (unsigned)raddr, (unsigned)k, rb(rdram, raddr + k), rb(rdram, raddr + k + 1), ref[k], ref[k + 1]);
                }
            }
        }
        if (bad_dmem >= 0 && *verbose > 0) {
            (*verbose)--;
            if (f.dump != nullptr) f.dump(i, w0, w1, pre_dm, dm, ref_dm);
            else generic_dump(f, i, w0, w1, pre_dm, dm, ref_dm);
        }
        // Continue from the reference's state.
        memcpy(dm, ref_dm, 0x1000);
        if (has_range) for (uint32_t k = 0; k < rlen; k++) rbw(rdram, raddr + k, ref[k]);
    }
    stats.tasks++;
    stats.diff_tasks++;
}

// ------------------------------------------------------------------ capture

namespace {
const char* g_capture_dir = nullptr;
int g_capture_first = 200;
}

void recomp3ds::audio_hle_capture_dir(const char* dir, int first_task) {
    g_capture_dir = dir;
    if (first_task > 0) g_capture_first = first_task;
}

bool recomp3ds::audio_hle_capturing() {
    return g_capture_dir != nullptr;
}

void recomp3ds::audio_hle_maybe_capture(const uint8_t* rdram, const OSTask& task) {
    static int seen = 0;
    if (g_capture_dir == nullptr) return;
    seen++;
    if (seen < g_capture_first || seen >= g_capture_first + 8) return;
    char path[192];
    snprintf(path, sizeof(path), "%s/audio_task_%d.bin", g_capture_dir, seen - g_capture_first);
    FILE* f = fopen(path, "wb");
    if (f == nullptr) { fprintf(stderr, "audio-hle: capture failed to open %s\n", path); return; }
    fwrite(&task, 1, sizeof(OSTask), f);
    for (uint32_t off = 0; off < 16u * 1024 * 1024; off += 65536) fwrite(rdram + off, 1, 65536, f);
    fclose(f);
    fprintf(stderr, "audio-hle: captured task %d to %s\n", seen, path);
}
