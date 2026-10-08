// The game synthesizer's reverb for the DSP synthesizer (dsp_synth.h).
// libaudio's effect (reverb.c's alFxPull, which n_audio keeps) is a delay
// line with sections, each reading two taps, mixing them with a feedforward
// and a feedback coefficient, low-passing and adding into one mono output
// heard on both sides. The voices send to it on the DSP's aux bus 0 (each
// channel's wet amount); the bus comes back to the CPU through ndsp's aux
// callback, the decoding thread runs the sections at the DSP's rate, and the
// callback hands the result back to the DSP, which mixes it into the output.
// The callback (on ndsp's thread) only copies; the reverb costs core 1 time.
// It is heard about two DSP frames (10 ms) later than on the N64.
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

// After ndspInit: registers the aux callback (the bus stays off until a
// configuration comes).
void dsp_reverb_init();
void dsp_reverb_shutdown();
// From any thread: the effect to run; count 0 turns the bus off. Taken up
// by the next dsp_reverb_process.
void dsp_reverb_configure(const ReverbConfig& c);
// Whether the voices should send to the aux bus (an effect is running).
bool dsp_reverb_active();
// The decoding thread, after each DSP frame: runs the reverb over what the
// bus brought back since the last call.
void dsp_reverb_process();
// Since the last call: ticks spent in dsp_reverb_process, and the times
// the callback found no output ready (the thread fell behind; silence then).
void dsp_reverb_take_stats(uint32_t* ticks, uint32_t* gaps);

}   // namespace recomp3ds

#endif
