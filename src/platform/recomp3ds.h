// recomp-3ds: the 3DS entry point for an N64ModernRuntime game.
#ifndef RECOMP3DS_H
#define RECOMP3DS_H

#include <cstdint>
#include <string>

#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"
#include "rt64_3ds.h"

namespace recomp3ds {

struct GameDesc {
    std::u8string game_id;              // u8"DK64": also the stored ROM name (<id>.z64) and save name
    const char* sd_dir = "";            // folder under sdmc:/3ds/ holding the ROM, saves and settings
    recomp::GameEntry entry;            // hash, save type, entrypoint, on_init_callback
    recomp::rsp::callbacks_t rsp{};     // get_rsp_microcode
    void (*register_overlays)() = nullptr;
    void (*register_patches)() = nullptr;
    std::string (*get_game_thread_name)(const OSThread* t) = nullptr;   // optional
    rt64_3ds::RenderDesc render;
    bool null_renderer = false;         // bring-up: accept display lists, draw nothing
    bool audio_hle = false;             // run the naudio audio task on the CPU (naudio_hle.cpp) instead of the recompiled RSP code
};

// Brings up the console, registers the game and runs the runtime. Returns the
// process exit code once the game has quit.
int run(const GameDesc& desc);

// Ask the runtime to shut the game down (from the HOME prompt or a script).
void request_exit();

// sdmc:/3ds/<sd_dir>
const char* base_path();

}   // namespace recomp3ds

#endif
