// A sampling profile of the game's recompiled code: which of its functions
// the game's core spends its time in, and how much of that time the core is
// idle (the game waiting on the renderer or the retrace).
//
// The game's trace.h (N64Recomp's trace_mode: TRACE_ENTRY at every
// function's start, TRACE_RETURN before every return) keeps
// `recomp3ds_trace_func` at the running function's name (__func__) and puts
// the caller's back on return, so the name is the function executing, not
// an ancestor: a self-time profile. A thread on the renderer's core reads it
// a thousand times a second, with the load monitor's idle count on the
// game's core since the last sample (an idle fraction of that millisecond),
// and the stats loop prints the top functions every 10 s and at exit.
// settings.ini's dbg_profile=2 turns it on (3 with the renderer's read
// profile); the per-call cost of the macros (a load, two stores) is paid
// either way.
#ifndef RECOMP3DS_GAME_PROFILE_H
#define RECOMP3DS_GAME_PROFILE_H

extern "C" {
extern const char* volatile recomp3ds_trace_func;
}

namespace recomp3ds {

// After the cores are chosen: the game's core (its idle count) and the core
// to sample from (the renderer's).
void game_profile_start(int game_core, int sampler_core);
bool game_profile_enabled();
// The top functions since the last report (final: since the start, more of
// them), as "game-profile:" log lines.
void game_profile_report(bool final);
void game_profile_stop();

}   // namespace recomp3ds

#endif
