// CPU implementation of the naudio audio microcode (n_aspMain family): the
// RSP audio task is interpreted at the command level instead of running the
// recompiled RSP code, which costs a whole ARM11 core when scalarised.
#ifndef RECOMP3DS_NAUDIO_HLE_H
#define RECOMP3DS_NAUDIO_HLE_H

#include <cstdint>

#include "librecomp/rsp.hpp"

namespace recomp3ds {

// Drop-in RspUcodeFunc: reads the OSTask that librecomp copied to DMEM 0xFC0.
RspExitReason naudio_hle_run(uint8_t* rdram, uint32_t ucode_addr);

// Differential mode: every task is also run through `reference` (the
// recompiled microcode) on the same inputs, and every RDRAM range the task
// writes is compared. Mismatches are logged; the reference's output is kept.
void naudio_hle_set_reference(RspUcodeFunc* reference);

struct NaudioHleStats {
    uint32_t tasks;
    uint32_t commands;
    uint32_t unknown_opcodes;
    uint32_t diff_tasks;        // tasks compared against the reference
    uint32_t diff_mismatches;   // ranges that differed
};
const NaudioHleStats& naudio_hle_stats();

}   // namespace recomp3ds

// Capture mode (see naudio_hle.cpp): directory that receives audio_task_N.bin.
void recomp3ds_naudio_capture_dir(const char* dir);

#endif
