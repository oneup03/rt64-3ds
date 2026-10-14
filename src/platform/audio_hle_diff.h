// What the audio microcode interpreters (naudio_hle.cpp, aspmain_hle.cpp)
// share: their statistics, the differential runner that checks one against
// the recompiled microcode command by command (the host tool's check), and task
// capture for the host harness (AUDIO_CAPTURE.TXT, tools/naudio_test).
#ifndef RECOMP3DS_AUDIO_HLE_DIFF_H
#define RECOMP3DS_AUDIO_HLE_DIFF_H

#include <cstdint>

#include "librecomp/rsp.hpp"

namespace recomp3ds {

struct AudioHleStats {
    uint32_t tasks;
    uint32_t commands;
    uint32_t unknown_opcodes;
    uint32_t diff_tasks;        // tasks compared against the reference
    uint32_t diff_mismatches;   // commands whose results differed
    // Commands that read a vector the microcode loads only in part (an
    // unaligned buffer), keeping lanes from earlier commands that the
    // interpreter does not track: their output may differ.
    uint32_t inexact;
    // Host ticks spent in each opcode (aspMain only; wrapping, a reader
    // subtracts its previous reading): which commands the task's time goes to.
    uint32_t op_ticks[16];
};

// An interpreter as the differential runner drives it.
struct AudioHleFamily {
    const char* log_name;       // prefix of its log lines ("naudio-hle")
    uint8_t* dm;                // its DMEM, in RDRAM byte order (byte a at [a ^ 3])
    AudioHleStats* stats;
    // Interprets one command on dm, as the microcode runs a task of that
    // command alone (the runner has pointed the task copy at 0xFC0 at it).
    void (*run_command)(uint8_t* rdram, uint32_t w0, uint32_t w1);
    // The RDRAM range the command writes, from dm as it is before the command.
    bool (*command_range)(uint32_t w0, uint32_t w1, uint32_t* addr, uint32_t* len);
    // DMEM bytes left out of the comparison (null: none), e.g. a command list
    // buffer the interpreter does not keep.
    bool (*dmem_ignored)(uint32_t a);
    // Optional (host tools): the whole picture of a DMEM mismatch, from the
    // DMEM before the command, the interpreter's after it and the reference's.
    void (*dump)(uint32_t index, uint32_t w0, uint32_t w1, const uint8_t* before, const uint8_t* mine, const uint8_t* ref);
    int reports_by_op[16];      // mismatches logged so far per opcode (three each)
};

// Runs a task command by command: each through `reference` (the recompiled
// microcode) on a task of that command alone, starting from the
// interpreter's DMEM, and through the interpreter; compares DMEM and the
// RDRAM the command writes, logs mismatches, and continues from the
// reference's results so one bug does not hide the next. `*verbose`
// mismatches are dumped in full (counted down).
void audio_hle_lockstep(AudioHleFamily& family, uint8_t* rdram, const OSTask& task, uint32_t ucode_addr,
                        RspUcodeFunc* reference, int* verbose);

// RSP DMA as librecomp implements it, on an interpreter's DMEM: an exact
// byte count, the RDRAM address aligned down to 8, DMEM wrapping at 4 KB.
void audio_hle_dma_read(uint8_t* dm, const uint8_t* rdram, uint32_t dmem, uint32_t dram, uint32_t len);
void audio_hle_dma_write(const uint8_t* dm, uint8_t* rdram, uint32_t dmem, uint32_t dram, uint32_t len);

// AUDIO_CAPTURE.TXT: from the `first_task`-th audio task (<= 0: the 200th)
// on, 8 tasks write their OSTask and the whole RDRAM to dir/audio_task_N.bin.
void audio_hle_capture_dir(const char* dir, int first_task);
bool audio_hle_capturing();
// Called with every audio task before it runs (the interpreter's or the
// recompiled microcode's).
void audio_hle_maybe_capture(const uint8_t* rdram, const OSTask& task);

}   // namespace recomp3ds

#endif
