// A game's synthesizer on the 3DS DSP. Every voice the game's synthesizer
// starts (libaudio's alSynStartVoice and the like) plays on an ndsp channel
// of its own: the DSP resamples it to the voice's pitch and mixes it with its
// volume and pan; the CPU only decodes the voice's samples from the ROM, a
// few buffers ahead. This replaces running the game's audio microcode (its
// RSP task is skipped and the AI stream carries silence), which on an Old
// 3DS takes most of a core.
//
// A game opts in with GameDesc::synth (the flavor of synthesizer, whose
// structures the hooks read, SynthDesc) and hooks its synthesizer's entry points in its
// recompiler config, each calling recomp3ds_synth_hook at the function's
// entry with the matching RECOMP3DS_SYNTH_* code (docs/porting-a-game.md).
// The game's synthesizer keeps running as before (its bookkeeping, voice
// allocation and timing are untouched); only its output is replaced.
//
// Timing: the hooks run while the game builds an audio frame, ahead of when
// that frame is heard. Each event is stamped with the synthesizer's sample
// clock and played when the AI stream (which still carries the game's
// frames, as silence) reaches that sample, so the latency and the rhythm
// stay the game's, to within a DSP frame (4.9 ms).
// The game's reverb runs on the DSP's aux bus (dsp_reverb.h).
#ifndef RECOMP3DS_DSP_SYNTH_H
#define RECOMP3DS_DSP_SYNTH_H

#include <cstdint>

namespace recomp3ds {

enum class SynthFlavor : uint8_t {
    None,           // the game's audio task runs as before
    Libaudio,       // libultra's libaudio: alSyn*(synth, voice, ...) (Goemon)
    NAudio,         // Rare's n_audio: n_alSyn*(voice, ...), the synth a global (Banjo, DK64)
};

// Where the game's effect (its reverb) is found from the aux bus.
enum class SynthFx : uint8_t {
    Embedded,       // libaudio: an ALFx at auxBus + 0x20
    Pointer,        // n_audio: an ALFx * at auxBus + 0x20
    Dk64,           // DK64's n_audio: an ALFx * at auxBus + 0x20, DK64's own ALFx layout
};

// The game's synthesizer as the hooks read it. A flavor gives its stock
// layout (`desc.synth = SynthFlavor::Libaudio`); a game whose build moved
// things changes the fields (Banjo's N_ALSynth has its clock at +0x28).
struct SynthDesc {
    SynthFlavor flavor = SynthFlavor::None;
    uint32_t synth_global = 0;      // NAudio: RDRAM address of the game's n_syn (a pointer to the synthesizer)
    uint16_t param_samples = 0x1C;  // the synthesizer's paramSamples, curSamples and auxBus
    uint16_t cur_samples = 0x20;
    uint16_t aux_bus = 0x34;
    uint16_t param_list = 0x2C;     // its free ALParam list and its lame physical voices (the
    uint16_t lame_list = 0x14;      // command-list skip's bookkeeping, recomp3ds_synth_skip_build)
    uint16_t pvoice_offset = 0xD8;  // the physical voice's steal delay ("offset"; n_audio 0x88)
    uint8_t start_t_stack = 28;     // StartVoiceParams' ramp time t, sp-relative (Banjo 24, DK64 36)
    SynthFx fx = SynthFx::Embedded;
    bool fx_lowpass = true;         // false: the game's microcode leaves its effect unfiltered (DK64)
    uint32_t sound_modes = 0;       // DK64: its surround, mono and headphone bytes (they move pan)
    SynthDesc() = default;
    SynthDesc(SynthFlavor f) : flavor(f) {
        if (f == SynthFlavor::NAudio) {
            pvoice_offset = 0x88;
            start_t_stack = 24;
            fx = SynthFx::Pointer;
        }
    }
};

// After audio_init: claims ndsp channels 1..23 and starts the decoding thread.
bool dsp_synth_init(const SynthDesc& desc);
// The settings menu's Audio engine row: true plays the game's voices on the
// DSP (and skips its audio task), false runs the task as before.
void dsp_synth_set_enabled(bool on);
bool dsp_synth_enabled();
void dsp_synth_shutdown();

struct DspSynthStats {
    uint32_t playing;       // channels playing now
    uint32_t starts;        // voices started in the interval
    uint32_t events;        // hook calls in the interval
    uint32_t decode_us;     // the thread's time in the interval (all of it)
    uint32_t decode_only_us;    // ... decoding samples
    uint32_t apply_us;          // ... applying events
    uint32_t starved;       // times a channel ran out of decoded samples
    uint32_t late;          // events applied after their time (ms behind, summed)
    uint32_t remaps;        // times the synthesizer's clock moved against the AI stream's
    uint32_t callbacks;     // ndsp callbacks (one a DSP frame, unless some ran late)
    uint32_t ndsp_dropped;  // DSP frames ndsp counted as missed
    uint32_t reverb_us;     // ... running the reverb (dsp_reverb.h; part of decode_us)
    uint32_t reverb_gaps;   // times the reverb's output was not ready in time
    uint32_t lists_skipped; // audio frames built without a command list (recomp3ds_synth_skip_build)
};
DspSynthStats dsp_synth_take_stats();

}   // namespace recomp3ds

// The hooks' entry point, C linkage for recompiled code: `ctx` is the
// recomp_context at the hooked function's entry (its arguments in a0..a3 and
// on the stack), `which` one of these.
// NAudio's are the same calls without the synthesizer (n_alSynSetVol(v,
// vol, delta), ...); DK64's StartVoiceParams has three more arguments
// before t (SynthDesc::start_t_stack).
enum {
    RECOMP3DS_SYNTH_START_VOICE = 0,        // alSynStartVoice(s, v, w)
    RECOMP3DS_SYNTH_START_VOICE_PARAMS = 1, // alSynStartVoiceParams(s, v, w, pitch, vol, pan, fxmix, t)
    RECOMP3DS_SYNTH_STOP_VOICE = 2,         // alSynStopVoice(s, v)
    RECOMP3DS_SYNTH_SET_VOL = 3,            // alSynSetVol(s, v, vol, delta)
    RECOMP3DS_SYNTH_SET_PITCH = 4,          // alSynSetPitch(s, v, ratio)
    RECOMP3DS_SYNTH_SET_PAN = 5,            // alSynSetPan(s, v, pan)
    RECOMP3DS_SYNTH_SET_FXMIX = 6,          // alSynSetFXMix(s, v, fxmix)
    RECOMP3DS_SYNTH_AUDIO_FRAME = 7,        // alAudioFrame(cmdList, cmdLen, outBuf, outLen)
};
extern "C" void recomp3ds_synth_hook(uint8_t* rdram, void* ctx, int which);

// The frame function's command building, skipped when nothing would run the
// list (the voices on the DSP, or Audio Off). A hook placed where the frame
// function (alAudioFrame, n_alAudioFrame) has run the players' handlers and
// is about to build passes the synthesizer (0: SynthDesc::synth_global's),
// the samples left to build and the list's end. When it returns non-zero it
// has taken the frame's updates as the envelope mixers would have (stops,
// frees, the parameters freed), moved the clock on and written one harmless
// command at the list's end: the hook then sets the samples left to 0 (the
// build loop runs no times) and moves the list's end past that command (an
// empty list means no task to an audio manager, and no buffer for the AI).
// The players' handlers, and with them the calls the DSP plays, run as
// before. Goemon (s1, s3, s4 before 0x80047B40):
//   if (recomp3ds_synth_skip_build(rdram, ctx->r17, ctx->r19, ctx->r20)) { ctx->r19 = 0; ctx->r20 += 8; }
extern "C" int recomp3ds_synth_skip_build(uint8_t* rdram, uint32_t synth, int32_t len, uint32_t list_end);

#endif
