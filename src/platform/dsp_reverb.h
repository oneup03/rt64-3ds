// The game synthesizer's reverb for the DSP synthesizer (dsp_synth.h).
// libaudio's effect (reverb.c's alFxPull, which n_audio keeps) is a delay
// line with sections, each reading two taps, mixing them with a feedforward
// and a feedback coefficient, low-passing and adding into one mono output
// heard on both sides. The voices send to the DSP's aux bus 0 (each
// channel's wet amount), where OpenDSP's firmware (third_party/OpenDSP) runs
// its own reverb: four diffusers and a four-line feedback network, set by
// room size, decay time, damping, diffusion, width and level. Nothing comes
// back to the CPU (libctru's ndsp never returns an aux bus), so the game's
// effect is not run itself but matched: when the game sets an effect, its
// sections are run here on an impulse, and the decay time, loudness, echo
// spacing and low-pass measured from that response choose OpenDSP's
// settings (a model of OpenDSP's network, run on the same impulse, sets the
// level). Without OpenDSP's firmware (the console's own from dspfirm.cdc)
// there is no reverb and the voices play dry.
#ifndef RECOMP3DS_DSP_REVERB_H
#define RECOMP3DS_DSP_REVERB_H

#include <cstdint>

namespace recomp3ds {

// The effect as the game's synthesizer set it up (ALFx), in its samples.
struct ReverbConfig {
    struct Section {
        uint32_t input = 0, output = 0;     // taps, samples behind the newest input
        int16_t ffcoef = 0, fbcoef = 0, gain = 0;   // Q15
        int16_t lp_gain = 0;    // the low-pass's input gain (Q14, ALLowPass::fgain); 0: none
        bool chorus = false;    // the output tap is modulated (and not written back)
    };
    uint32_t length = 0;        // the delay line, samples
    uint32_t rate = 0;          // the synthesizer's output rate
    uint8_t count = 0;          // sections
    Section sec[8];
    bool same(const ReverbConfig& o) const;
};

// After ndspInit: the bus off until a configuration comes.
void dsp_reverb_init();
void dsp_reverb_shutdown();
// From any thread: the effect to run; count 0 turns it off. Taken up by the
// next dsp_reverb_process.
void dsp_reverb_configure(const ReverbConfig& c);
// Whether the voices should send to the aux bus (OpenDSP's reverb runs).
bool dsp_reverb_active();
// The effect's early part played dry: this much of a voice's wet send added
// to its dry level, as energy (ReverbMatch::early; 0 without the reverb).
float dsp_reverb_early();
// Changes whenever the active effect does (the voices' mixes to re-apply).
uint32_t dsp_reverb_generation();
// The decoding thread, after each DSP frame: a new configuration, if any,
// matched and handed to the firmware.
void dsp_reverb_process();
// Since the last call: ticks spent matching configurations, and gaps (none
// since the reverb runs on the DSP; kept for the stats line).
void dsp_reverb_take_stats(uint32_t* ticks, uint32_t* gaps);

}   // namespace recomp3ds

#endif
