// recomp-3ds: the 3DS entry point for an N64ModernRuntime game.
#ifndef RECOMP3DS_H
#define RECOMP3DS_H

#include <cstdint>
#include <string>

#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"
#include "ultramodern/input.hpp"
#include "naudio_hle.h"
#include "dsp_synth.h"
#include "aspmain_hle.h"
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

// Which audio microcode the game's audio task is, for GameDesc::audio_hle:
// the naudio family (n_aspMain: DK64, Banjo; naudio_hle.cpp) or libaudio's
// ABI 1 aspMain (Goemon; aspmain_hle.cpp). They differ in their command
// numbers and DMEM layout, so the wrong one garbles every sound.
enum class AudioHle : uint8_t { Naudio, Aspmain };

// The settings menu's pages; the stereo rows are on Page3D and the stick
// deadzones on PageControls. Empty pages are skipped.
enum MenuPage : uint8_t { Page3D, PageControls, PageCamera, PageGame, PageMods, PageCount };

// A game setting on the touch-screen menu, saved to settings.ini under `key`.
struct MenuOption {
    const char* key;
    const char* label;                  // up to 25 characters
    int lo, hi;
    int* value;                         // holds the default until settings.ini is read
    const char* const* names = nullptr; // hi - lo + 1 names (up to 12 characters) instead of the number
    void (*on_change)(int value) = nullptr;   // also called once after loading
    MenuPage page = PageGame;
    int step = 1;
    const char* suffix = nullptr;       // after the number ("%")
    const char* zero_text = nullptr;    // shown for 0 instead of the number ("Off")
};

// A game's starting value for a menu row by its key, the library's own rows
// included (3D depth "separation", "convergence_tenths",
// "stereo_ghost_contrast", "stick_deadzone", "cstick_deadzone",
// "cstick_up", ...); settings.ini, once written, overrides it.
struct MenuDefault {
    const char* key;
    int value;
    // Only on one kind of console (a log level that an Old 3DS keeps lower,
    // say); later entries win.
    enum : uint8_t { AnyConsole, New3dsOnly, Old3dsOnly } console = AnyConsole;
};

struct GameDesc {
    std::u8string game_id;              // u8"DK64": also the stored ROM name (<id>.z64) and save name
    const char* sd_dir = "";            // folder under sdmc:/3ds/ holding the ROM, saves and settings
    recomp::GameEntry entry;            // hash, save type, entrypoint, on_init_callback
    recomp::Version project_version{};  // the game's version, which mods check (minimum_recomp_version)
    // Mods baked in that are part of the game: on whatever settings.ini says,
    // no on/off row on the Mods page (their options still show), and a
    // message plus exit when one's file is not on the card (a mod without
    // :embed needs its .nrm in mods/). Ids as in their manifests.
    const char* const* required_mods = nullptr;
    size_t required_mod_count = 0;
    const char* required_mods_hint = nullptr;   // appended to the missing-file message (where to get it)
    // The ROM stream's cache (rom_stream_install): its size and its block, the
    // unit of one card read. A game that loads long runs (Dinosaur Planet's
    // maps) spends 10 ms per read on the card whatever the size, so bigger
    // blocks load faster; small random reads cost a block each.
    uint32_t rom_cache_mb = 2;
    uint32_t rom_block_kb = 128;
    // Built-in mods (RT64_3DS_STATIC_MODS) left out when a game mode is
    // played, whatever is enabled: ones that replace a function the game
    // modes replace too (the mode would not load), or that assume the
    // original game's maps. The Mods page hides them during a game mode.
    const char* const* mods_not_in_game_modes = nullptr;
    size_t mods_not_in_game_modes_count = 0;
    // Optional: a mod was switched on or off on the Mods page (it takes
    // effect at once), e.g. to give a mod's key a button.
    void (*on_mod_switched)(const char* mod_id, bool on) = nullptr;
    recomp::rsp::callbacks_t rsp{};     // get_rsp_microcode
    void (*register_overlays)() = nullptr;
    void (*register_patches)() = nullptr;
    std::string (*get_game_thread_name)(const OSThread* t) = nullptr;   // optional
    rt64_3ds::RenderDesc render;
    bool null_renderer = false;         // bring-up: accept display lists, draw nothing
    bool audio_hle = false;             // run the audio task on the CPU (audio_hle_family) instead of the recompiled RSP code
    recomp3ds::AudioHle audio_hle_family = recomp3ds::AudioHle::Naudio;           // which microcode the audio task is
    // How many video frames of audio the game is told are queued fewer than
    // really are, so it keeps that much more in reserve (more latency). A game
    // that sizes its audio from the queue, with little slack of its own, lets
    // the DSP run dry between buffers otherwise: short gaps heard as a constant
    // crackle, and the frames it submits per second fall short of the rate.
    int audio_margin_frames = 1;
    // The audio task's completion is reported at once and its samples are
    // passed on when it has really run (ultramodern::set_async_audio_tasks):
    // for a game with an SDK-style audio manager, whose next task waits for
    // the previous one's completion and so falls to every other retrace when
    // the interpreter takes most of one. Needs every task's output to reach
    // the host through osAiSetNextBuffer alone.
    bool audio_async = false;
    // With two cores (an Old 3DS, or the Cores row's two-core layouts on a
    // New 3DS): the game on core 0, which the application has whole, and the
    // renderer, the audio task and the synthesizer on core 1 (its share,
    // 80%). Off: the game on core 1 and the rest on core 0. The larger load
    // wants core 0: Goemon's renderer (24.4 fps at the Old clock against 17.5
    // the other way round), Banjo's game (its core full at 13-20 fps).
    bool two_cores_game_on_core0 = false;
    // With two cores and the game on core 0, for a game that waits on
    // "parsed" every frame (Banjo's game_draw): each display list is read on
    // core 0, idle while the game waits for exactly that, and drawn on core 1
    // (rt64_3ds::Settings::read_core). Banjo's read took 26 ms on core 1
    // against about 21 on core 0, all of it in the game's frame; with the
    // triangles' output on core 1 too (Interpreter::set_defer_triangles),
    // 8% more frames at the Old clock.
    bool two_cores_read_on_game_core = false;
    // The game's synthesizer on the DSP (dsp_synth.h): the flavor of its
    // synthesizer, whose entry points the game's recompiler config hooks
    // (recomp3ds_synth_hook). Offers the Audio engine row (RSP / DSP).
    SynthDesc synth = SynthFlavor::None;
    recomp3ds::NaudioVariant audio_hle_variant = recomp3ds::NaudioVariant::Dk64;   // naudio: which build of n_aspMain the game has
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
    // Optional: the game's defaults for rows, the library's included.
    const MenuDefault* menu_defaults = nullptr;
    size_t menu_default_count = 0;
    // What a default of "60 interp." (frame_cap 2) becomes on an Old 3DS,
    // which can't afford the in-betweens: 1, the 30 fps cap (60 interp.
    // without the frames in between; Goemon), or 0, the game's own rate
    // (DK64, Banjo). settings.ini still decides.
    int old3ds_frame_cap = 1;
    // What the runtime reports plugged into controller 1. Goemon saves to
    // the Controller Pak and refuses to start a game unless one is reported
    // at boot (the desktop frontends report a Rumble Pak).
    ultramodern::input::Pak controller_pak = ultramodern::input::Pak::None;
    // Optional: the C-Stick as the game's own button camera, for games whose
    // camera turns while a button is held and C buttons are pressed, one
    // step a press (Goemon: hold R, C-Left/Right turn 45 degrees, C-Up/Down
    // zoom; the camera eases back behind the player once the presses stop).
    // While the C-Stick points a way (its stronger axis), it holds these N64
    // buttons and, once they have been held cstick_camera_lead_ms (the game
    // must see them first), presses that way's C button for
    // cstick_camera_press_ms every cstick_camera_period_ms, so holding the
    // stick keeps turning. 0: the C-Stick presses the C buttons, as ever.
    // The Controls page gets a "C-Stick" row (Camera / C buttons).
    uint16_t cstick_camera_hold = 0;
    uint16_t cstick_camera_lead_ms = 70;
    uint16_t cstick_camera_press_ms = 100;
    uint16_t cstick_camera_period_ms = 267;
    // Optional: called once the audio is up (ndsp, and the DSP synthesizer
    // when the game has one) with the first ndsp channel the platform leaves
    // free (23 - SynthDesc::reserve_channels + 1; 24 when none is), for a
    // game that plays something of its own on a channel (Dinosaur Planet's
    // MP3 voice acting).
    void (*on_audio_ready)(int first_free_channel) = nullptr;
    // Where the ROM lives, chosen by the game for each console: held in
    // memory, read from the SD card as the game needs it (the ROM stream,
    // rom_cache_mb / rom_block_kb), or Auto: in memory when the heap has
    // room for it beside RDRAM and rom_room_mb, else streamed. settings.ini's
    // rom_stream (1 in memory, 2 the card) overrides both; 0 is the game's
    // choice.
    enum class RomPlace : uint8_t { Auto, Memory, Stream };
    RomPlace rom_new3ds = RomPlace::Auto;
    RomPlace rom_old3ds = RomPlace::Auto;
    // The room Auto wants beside the ROM and RDRAM in the application heap
    // (recomp3ds_run.cpp; 24 MB by default). A game whose ROM only fits with
    // less (Dinosaur Planet's 64 MB ROM with a 16 MB linear heap: ~20 MB
    // left) lowers it; the log's "heap in use" says what the game really
    // takes.
    int rom_room_mb = 24;
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
// N64 buttons (N64_DUP ... N64_L) to offer on the touch screen while the
// settings menu is closed, for mods that read buttons the game's own map
// leaves out; 0 hides them. Any thread.
void touch_pad_set(uint16_t n64_buttons);

// Swap the button map while running (a setting that repurposes buttons).
// The array must stay alive; GameDesc.button_map is the one at start.
void input_set_map(const ButtonMap* map, size_t count);

// The gyroscope (and accelerometer), for games that aim with it: turned on
// only while wanted (they cost power). The degrees the console turned right
// (about gravity) and tilted up since the last call, calibrated.
void input_set_gyro(bool on);
void input_take_gyro(float* turn_right_deg, float* look_up_deg);

}   // namespace recomp3ds

#endif
