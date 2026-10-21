// OpenDSP's reverb settings for a game's effect (dsp_reverb.h): measured
// from the effect's response to an impulse and from a model of OpenDSP's
// network. Plain C++, no 3DS headers, so the host test
// (tests/reverb_match) builds the same code.
#ifndef RECOMP3DS_DSP_REVERB_MATCH_H
#define RECOMP3DS_DSP_REVERB_MATCH_H

#include <vector>
#include "dsp_reverb.h"

namespace recomp3ds {

// opendspReverbConfig's fields (opendsp/effects.h), and the bus's return.
// An effect that is one echo (a single section feeding back on itself,
// Goemon's) goes to OpenDSP's delay instead (opendspDelayConfig): its
// reverb's lines (54 ms at most) would smear echoes apart by more.
struct ReverbMatch {
    float room_size = 0.0f, decay_time = 0.0f, damping = 0.0f, diffusion = 0.0f, width = 0.0f, level = 0.0f;
    float volume = 1.0f;        // ndspAuxSetVolume: past level 1
    bool echo = false;          // the delay, not the reverb
    float delay_ms = 0.0f, feedback = 0.0f;     // the delay's (with damping and level)
    // The effect's first 40 ms (DK64: over half its energy, mostly a short
    // all-pass, a near copy of the dry sound): played dry, this much of a
    // voice's wet send added to its dry level (as energy). OpenDSP's
    // reverb, all late, matches what comes after.
    float early = 0.0f;
};

// A response's energy per 32 samples (at the DSP's rate), its decay and its whole
// energy (one side, per unit impulse sent equally to the bus's left and right).
struct ReverbResponse {
    std::vector<double> block;
    double energy = 0.0;
    double rt60 = 0.0;          // seconds
};

// Fills energy and rt60 from block (Schroeder's backward integration).
void reverb_measure(ReverbResponse& r);
// The game's effect (alFxPull's sections) at the DSP's rate.
ReverbResponse reverb_n64_response(const ReverbConfig& c);
// OpenDSP's network as opendspReverbSet sets it (the diffusers left out:
// they keep the energy; the loops' all-passes as their mean delay).
ReverbResponse reverb_opendsp_response(const ReverbMatch& m);
// The settings, and both responses as matched (for the log).
ReverbMatch reverb_match(const ReverbConfig& c, ReverbResponse* n64 = nullptr, ReverbResponse* matched = nullptr);

}   // namespace recomp3ds

#endif
