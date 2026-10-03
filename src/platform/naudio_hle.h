// CPU implementation of the naudio audio microcode (n_aspMain family): the
// RSP audio task is interpreted at the command level instead of running the
// recompiled RSP code, which costs a whole ARM11 core when scalarised.
#ifndef RECOMP3DS_NAUDIO_HLE_H
#define RECOMP3DS_NAUDIO_HLE_H

#include <cstdint>

#include "librecomp/rsp.hpp"
#include "audio_hle_diff.h"

namespace recomp3ds {

// Drop-in RspUcodeFunc: reads the OSTask that librecomp copied to DMEM 0xFC0.
RspExitReason naudio_hle_run(uint8_t* rdram, uint32_t ucode_addr);

// Builds of n_aspMain differ in small ways. DK64's envelope mixer inverts a
// voice's left input when the dry volume's low bit is set and the right one
// on the wet volume's; Banjo-Kazooie's (an older build) has no inversion.
enum class NaudioVariant : uint8_t { Dk64, BanjoKazooie };
void naudio_hle_set_variant(NaudioVariant variant);

// Differential mode (audio_hle_lockstep): every command also runs through
// `reference` (the recompiled microcode) on the same inputs, and DMEM and the
// RDRAM it writes are compared. Mismatches are logged; the reference's output
// is kept.
void naudio_hle_set_reference(RspUcodeFunc* reference);
// Host tools: dump the next `mismatches` differential mismatches in full.
void naudio_hle_set_diff_verbose(int mismatches);

using NaudioHleStats = AudioHleStats;
const AudioHleStats& naudio_hle_stats();
// Host tools: the interpreter's DMEM (RDRAM byte order), and clearing it.
const uint8_t* naudio_hle_dmem();
void naudio_hle_reset_dmem();

}   // namespace recomp3ds

#endif
