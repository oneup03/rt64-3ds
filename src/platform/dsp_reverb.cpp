// The synthesizer's reverb on OpenDSP's aux bus 0 (dsp_reverb.h).
#include "dsp_reverb.h"
#include "dsp_reverb_match.h"
#include "recomp3ds.h"
#include "recomp3ds_internal.h"

#include <3ds.h>
#include <opendsp.h>
#include <opendsp/effects.h>
#include <cstdio>

namespace {

volatile bool g_running = false;        // OpenDSP's reverb runs (the voices send)
volatile float g_early = 0.0f;          // ReverbMatch::early of the running effect
volatile uint32_t g_generation = 0;     // bumped on each change of the above
bool g_has_reverb = false;              // OpenDSP's firmware with its reverb (dsp_reverb_init)
volatile uint32_t g_ticks = 0;

// The configuration the hooks last set, taken up by the thread.
LightLock g_lock;
recomp3ds::ReverbConfig g_pending;
bool g_has_pending = false;
recomp3ds::ReverbConfig g_config;       // the thread's

void apply(const recomp3ds::ReverbConfig& c) {
    g_config = c;
    // The game's effect as set (each change), with or without OpenDSP.
    fprintf(stderr, "dsp-reverb: effect %u sections, %lu samples at %lu Hz\n", (unsigned)c.count, (unsigned long)c.length, (unsigned long)c.rate);
    for (int i = 0; i < c.count; i++) {
        const recomp3ds::ReverbConfig::Section& s = c.sec[i];
        fprintf(stderr, "dsp-reverb:   %d: taps %lu-%lu ff %d fb %d gain %d lp %d%s\n", i, (unsigned long)s.input, (unsigned long)s.output,
                s.ffcoef, s.fbcoef, s.gain, s.lp_gain, s.chorus ? " chorus" : "");
    }
    if (c.count == 0 || c.rate == 0 || !g_has_reverb) {
        if (g_running) {
            opendspReverbSet(0, nullptr);
            opendspDelaySet(0, nullptr);
            g_running = false;
            g_early = 0.0f;
            g_generation = g_generation + 1;
        }
        return;
    }
    recomp3ds::ReverbResponse n64, matched;
    const recomp3ds::ReverbMatch m = recomp3ds::reverb_match(c, &n64, &matched);
    if (m.echo) {
        // One echo: OpenDSP's delay, its reverb off.
        fprintf(stderr, "dsp-reverb: one echo, RT60 %.2f s -> OpenDSP delay %.1f ms feedback %.2f damping %.2f level %.2f return %.2f\n",
                n64.rt60, m.delay_ms, m.feedback, m.damping, m.level, m.volume);
        opendspReverbSet(0, nullptr);
        opendspDelayConfig dcfg = {};
        dcfg.struct_size = sizeof(dcfg);
        dcfg.delay_ms = m.delay_ms;
        dcfg.feedback = m.feedback;
        dcfg.damping = m.damping;
        dcfg.level = m.level;
        if (R_FAILED(opendspDelaySet(0, &dcfg))) {
            fprintf(stderr, "dsp-reverb: opendspDelaySet failed; the voices play dry\n");
            g_running = false;
            return;
        }
        ndspAuxSetVolume(0, m.volume);
        g_running = true;
        g_early = 0.0f;
        g_generation = g_generation + 1;
        return;
    }
    fprintf(stderr, "dsp-reverb: RT60 %.2f s, energy %.3f -> early %.2f dry; OpenDSP room %.2f decay %.2f s damping %.2f diffusion %.2f level %.2f "
            "return %.2f (model: RT60 %.2f s, energy %.3f)\n", n64.rt60, n64.energy, m.early, m.room_size, m.decay_time, m.damping, m.diffusion,
            m.level, m.volume, matched.rt60, matched.energy);
    opendspDelaySet(0, nullptr);
    opendspReverbConfig cfg = {};
    cfg.struct_size = sizeof(cfg);
    cfg.room_size = m.room_size;
    cfg.decay_time = m.decay_time;
    cfg.damping = m.damping;
    cfg.diffusion = m.diffusion;
    cfg.width = m.width;
    cfg.level = m.level;
    if (R_FAILED(opendspReverbSet(0, &cfg))) {
        fprintf(stderr, "dsp-reverb: opendspReverbSet failed; the voices play dry\n");
        g_running = false;
        return;
    }
    ndspAuxSetVolume(0, m.volume);
    g_running = true;
    g_early = m.early;
    g_generation = g_generation + 1;
}

}   // namespace

bool recomp3ds::ReverbConfig::same(const ReverbConfig& o) const {
    if (count != o.count || length != o.length || rate != o.rate) return false;
    for (int i = 0; i < count; i++) {
        const Section& a = sec[i];
        const Section& b = o.sec[i];
        if (a.input != b.input || a.output != b.output || a.ffcoef != b.ffcoef || a.fbcoef != b.fbcoef || a.gain != b.gain ||
            a.lp_gain != b.lp_gain || a.chorus != b.chorus) return false;
    }
    return true;
}

void recomp3ds::dsp_reverb_init() {
    LightLock_Init(&g_lock);
    // The bus is OpenDSP's effect's input: never the ARM's (ndspAuxSetEnable
    // would cut the effect off).
    ndspAuxSetEnable(0, false);
    u32 caps = 0;
    g_has_reverb = recomp3ds::audio_opendsp() && R_SUCCEEDED(opendspGetCapabilities(&caps)) && (caps & OPENDSP_CAP_REVERB) != 0;
    fprintf(stderr, "dsp-reverb: %s\n", g_has_reverb ? "OpenDSP's reverb on aux bus 0" : "no DSP reverb (the voices play dry)");
}

void recomp3ds::dsp_reverb_shutdown() {
    if (g_running) {
        opendspReverbSet(0, nullptr);
        opendspDelaySet(0, nullptr);
    }
    g_running = false;
}

void recomp3ds::dsp_reverb_configure(const ReverbConfig& c) {
    LightLock_Lock(&g_lock);
    if (!c.same(g_pending) || !g_has_pending) {
        g_pending = c;
        g_has_pending = true;
    }
    LightLock_Unlock(&g_lock);
}

bool recomp3ds::dsp_reverb_active() { return g_running; }
float recomp3ds::dsp_reverb_early() { return g_running ? g_early : 0.0f; }
uint32_t recomp3ds::dsp_reverb_generation() { return g_generation; }

void recomp3ds::dsp_reverb_process() {
    LightLock_Lock(&g_lock);
    const bool take = g_has_pending;
    recomp3ds::ReverbConfig c;
    if (take) { c = g_pending; g_has_pending = false; }
    LightLock_Unlock(&g_lock);
    if (!take || c.same(g_config)) return;
    const u64 t0 = svcGetSystemTick();
    apply(c);
    g_ticks = g_ticks + (uint32_t)(svcGetSystemTick() - t0);
}

void recomp3ds::dsp_reverb_take_stats(uint32_t* ticks, uint32_t* gaps) {
    *ticks = g_ticks;
    *gaps = 0;
    g_ticks = 0;
}
