// recomp-3ds: the 3DS entry point for an N64ModernRuntime game.
#ifndef RECOMP3DS_H
#define RECOMP3DS_H

#include <cstdint>
#include <string>

#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"
#include "rt64_3ds.h"

namespace recomp3ds {

// N64 controller button bits (OSContPad.button).
enum : uint16_t {
    N64_A = 0x8000, N64_B = 0x4000, N64_Z = 0x2000, N64_START = 0x1000,
    N64_DUP = 0x0800, N64_DDOWN = 0x0400, N64_DLEFT = 0x0200, N64_DRIGHT = 0x0100,
    N64_L = 0x0020, N64_R = 0x0010,
    N64_CUP = 0x0008, N64_CDOWN = 0x0004, N64_CLEFT = 0x0002, N64_CRIGHT = 0x0001,
};

// One line of a button map: any of these 3DS keys (libctru KEY_*) presses
// these N64 buttons. The Circle Pad is always the stick and the C-Stick the
// C buttons (see input_set_cstick_buttons).
struct ButtonMap {
    uint32_t keys;
    uint16_t n64;
};

// The settings menu's pages; the stereo rows are on Page3D and the stick
// deadzones on PageControls. Empty pages are skipped.
enum MenuPage : uint8_t { Page3D, PageControls, PageCamera, PageGame, PageCount };

// A game setting on the touch-screen menu, saved to settings.ini under `key`.
struct MenuOption {
    const char* key;
    const char* label;                  // up to 13 characters
    int lo, hi;
    int* value;                         // holds the default until settings.ini is read
    const char* const* names = nullptr; // hi - lo + 1 names (up to 11 characters) instead of the number
    void (*on_change)(int value) = nullptr;   // also called once after loading
    MenuPage page = PageGame;
    int step = 1;
    const char* suffix = nullptr;       // after the number ("%")
    const char* zero_text = nullptr;    // shown for 0 instead of the number ("Off")
};

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
    // Called once when audio tasks stop arriving for 2 s after having run:
    // the game can log its scheduler/audio state (debugging lost wake-ups).
    void (*on_audio_stall)(uint8_t* rdram) = nullptr;
    // Optional: game-specific numbers appended to the once-a-second stats
    // line (e.g. the game's own VI counter and frame pacing).
    void (*append_stats)(const uint8_t* rdram, char* buf, size_t size) = nullptr;
    // Optional: the game's button map (null: the default in input_hid.cpp).
    const ButtonMap* button_map = nullptr;
    size_t button_map_count = 0;
    // Optional: game settings shown on the menu after the stereo ones.
    const MenuOption* menu_options = nullptr;
    size_t menu_option_count = 0;
};

// Brings up the console, registers the game and runs the runtime. Returns the
// process exit code once the game has quit.
int run(const GameDesc& desc);

// Ask the runtime to shut the game down (from the HOME prompt or a script).
void request_exit();

// sdmc:/3ds/<sd_dir>
const char* base_path();

// Whether the C-Stick also presses the C buttons (on by default). A game
// that reads the C-Stick as an analog camera turns it off.
void input_set_cstick_buttons(bool on);

// The gyroscope (and accelerometer), for games that aim with it: turned on
// only while wanted (they cost power). The degrees the console turned right
// (about gravity) and tilted up since the last call, calibrated.
void input_set_gyro(bool on);
void input_take_gyro(float* turn_right_deg, float* look_up_deg);

}   // namespace recomp3ds

#endif
