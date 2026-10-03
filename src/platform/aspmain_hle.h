// CPU implementation of libaudio's ABI 1 audio microcode (aspMain, "audio
// 1.0/2.0": A_SEGMENT 7, A_SETBUFF 8, A_POLEF 14), as in Mystical Ninja
// Starring Goemon. Like naudio_hle.cpp for the n_aspMain family it
// interprets the task's command list bit for bit instead of running the
// recompiled RSP code, which costs most of an ARM11 core when scalarised.
// The two families number and lay out their commands differently: pick the
// one the game's microcode is (GameDesc::audio_hle_family).
#ifndef RECOMP3DS_ASPMAIN_HLE_H
#define RECOMP3DS_ASPMAIN_HLE_H

#include <cstdint>

#include "librecomp/rsp.hpp"
#include "audio_hle_diff.h"

namespace recomp3ds {

// Drop-in RspUcodeFunc: reads the OSTask that librecomp copied to DMEM 0xFC0.
RspExitReason aspmain_hle_run(uint8_t* rdram, uint32_t ucode_addr);

// Differential mode (audio_hle_lockstep), as naudio_hle_set_reference.
void aspmain_hle_set_reference(RspUcodeFunc* reference);
// Host tools: dump the next `mismatches` differential mismatches in full.
void aspmain_hle_set_diff_verbose(int mismatches);

const AudioHleStats& aspmain_hle_stats();
// Low quality, for a CPU that cannot run the task in time (an Old 3DS: 10 to
// 17 ms a task at 44.1 kHz with eight voices): each sub-frame is resampled
// and mixed at half the sample rate, the reverb is left out (its delay lines
// are written silent) and the result is upsampled once where the channels
// are interleaved. About half the work. Not bit-exact, by design; what the
// commands save to RDRAM between tasks keeps its form, so it can be switched
// while the game runs.
void aspmain_hle_set_low_quality(bool on);
bool aspmain_hle_low_quality();
// The reverb alone left out, at the full rate (host tools: to tell what Low
// changes by leaving the reverb out from what its half rate changes).
void aspmain_hle_set_no_reverb(bool on);

// Host tools: the interpreter's DMEM (RDRAM byte order), and clearing it, so
// that a run starts as a fresh RSP would (some commands read what earlier
// tasks left behind, as the microcode does).
const uint8_t* aspmain_hle_dmem();
void aspmain_hle_reset_dmem();

}   // namespace recomp3ds

#endif
