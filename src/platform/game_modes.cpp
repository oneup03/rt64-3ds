// Game modes (RT64_3DS_STATIC_MODS): mods that turn the game into another
// one, a ROM hack plus its recompiled code. The code is baked into the
// executable; the player brings the mod's .nrm, and the hack's ROM made from
// it on a PC (rt64-3ds tools/make_game_mode_rom.py) as
// modes/<mod id>.z64: its patch.bps makes a 64 MB ROM, and the 13-14 MB
// patch alone is more than an Old 3DS has room for beside RDRAM. Before the
// game starts a picker on the bottom screen offers the game and every game
// mode it can play, and lists the others with the reason (the .nrm or its
// ROM is missing, or either is another version than the one built in, or the
// mode isn't built in at all). The runtime then asks use_mode_rom for the
// chosen mode's ROM (recomp::mods::set_rom_file_handler, nmr 0033), never
// reading the patch, and the ROM is streamed from the card.
#include <3ds.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unordered_set>
#include <vector>

#include "recomp3ds.h"
#include "recomp3ds_internal.h"

#ifdef RECOMP_STATIC_MODS
#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "miniz.h"

namespace {

std::string g_base_path;
const recomp3ds::GameDesc* g_desc = nullptr;

// The .nrm's patch.bps sizes, from the zip's directory: nothing is
// extracted. (Not its CRC32: a BPS patch ends with its own CRC, so the
// whole file's is always the same.)
bool patch_sizes(const std::filesystem::path& nrm, uint64_t& size, uint64_t& packed) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, nrm.string().c_str(), 0)) return false;
    const int index = mz_zip_reader_locate_file(&zip, "patch.bps", nullptr, 0);
    mz_zip_archive_file_stat st;
    const bool ok = index >= 0 && mz_zip_reader_file_stat(&zip, (mz_uint)index, &st);
    if (ok) { size = st.m_uncomp_size; packed = st.m_comp_size; }
    mz_zip_reader_end(&zip);
    return ok;
}

std::string mode_rom_path(const std::string& mod_id) { return g_base_path + "/modes/" + mod_id + ".z64"; }

// Whether modes/<mod id>.z64 was made from this .nrm's patch: the script
// writes "<patch.bps's size> <its compressed size> <the ROM's size>" next
// to it (<mod id>.z64.ok). Otherwise *why says what to do.
bool mode_rom_ready(const std::string& mod_id, const std::filesystem::path& nrm, std::string* why) {
    const std::string rom = mode_rom_path(mod_id);
    struct stat st;
    char line[64] = "";
    FILE* f = fopen((rom + ".ok").c_str(), "r");
    const bool stamped = f != nullptr && fgets(line, sizeof(line), f) != nullptr;
    if (f != nullptr) fclose(f);
    unsigned long long patch_made = 0, packed_made = 0, size_made = 0;
    if (stat(rom.c_str(), &st) != 0 || !stamped || sscanf(line, "%llu %llu %llu", &patch_made, &packed_made, &size_made) != 3) {
        if (why) *why = "needs its ROM: make_game_mode_rom.py";
        return false;
    }
    uint64_t patch = 0, packed = 0;
    if (!patch_sizes(nrm, patch, packed)) {
        if (why) *why = "its .nrm has no patch.bps";
        return false;
    }
    if (patch != patch_made || packed != packed_made || (unsigned long long)st.st_size != size_made) {
        if (why) *why = "ROM from another .nrm: run the script again";
        return false;
    }
    return true;
}

bool use_mode_rom(const std::string& mod_id, const std::filesystem::path& nrm) {
    std::string why;
    if (!mode_rom_ready(mod_id, nrm, &why)) {
        fprintf(stderr, "recomp3ds: %s %s\n", mod_id.c_str(), why.c_str());
        return false;
    }
    if (recomp::get_rom_stream() == nullptr) recomp3ds::rom_stream_install((size_t)g_desc->rom_cache_mb << 20, (size_t)g_desc->rom_block_kb << 10);
    if (!recomp::open_rom_stream(mode_rom_path(mod_id))) return false;
    recomp::set_rom_contents({});
    return true;
}

}   // namespace

void recomp3ds::game_modes_init(const char* base_path, const GameDesc& desc) {
    g_base_path = base_path;
    g_desc = &desc;
    recomp::mods::set_rom_file_handler(use_mode_rom);
}

// What mod_bake writes into the game's static_mods.cpp (null-terminated);
// weak, for a build without it.
extern "C" {
struct recomp_baked_game_mode { const char* mod_id; const char* display_name; const char* version; const char* file_name; };
__attribute__((weak)) extern const recomp_baked_game_mode recomp_baked_game_modes[];
}

std::string recomp3ds::pick_game_mode(const GameDesc& desc) {
    // The runtime scans again when it starts; this early scan finds the modes.
    recomp::mods::initialize_mods();
    recomp::mods::scan_mods();
    struct Choice {
        std::string id, name, note;     // a note says why it can't be played
    };
    std::vector<Choice> choices{ { "", desc.entry.display_name, "" } };
    std::vector<bool> seen;
    const recomp_baked_game_mode* baked = recomp_baked_game_modes;
    size_t baked_count = 0;
    while (baked != nullptr && baked[baked_count].mod_id != nullptr) baked_count++;
    seen.resize(baked_count);
    for (const recomp::mods::ModDetails& d : recomp::mods::get_all_mod_details(desc.entry.mod_game_id)) {
        if (!d.custom_gamemode) continue;
        Choice c{ d.mod_id, d.display_name, "" };
        size_t b = 0;
        while (b < baked_count && d.mod_id != baked[b].mod_id) b++;
        if (b == baked_count) c.note = "not built into this version";
        else {
            seen[b] = true;
            // The runtime checks the mod's code against the baked one too,
            // but only once it loads: a wrong version stops here.
            recomp::Version want;
            recomp::Version::from_string(baked[b].version, want);
            if ((d.version <=> want) != 0 || d.version.suffix != want.suffix) {
                c.note = "is " + d.version.to_string() + ", needs " + want.to_string();
            }
            else {
                std::string why;
                if (!mode_rom_ready(d.mod_id, d.mod_path, &why)) c.note = why;
            }
        }
        choices.push_back(std::move(c));
    }
    for (size_t b = 0; b < baked_count; b++) {
        if (!seen[b]) choices.push_back({ baked[b].mod_id, baked[b].display_name, std::string("needs mods/") + baked[b].file_name });
    }
    if (choices.size() == 1) return {};
    for (const Choice& c : choices) {
        if (!c.note.empty()) fprintf(stderr, "recomp3ds: game mode %s can't be played: %s\n", c.name.c_str(), c.note.c_str());
    }

    // The game first, or the last mode played if it can be played.
    const std::string last = recomp3ds::settings_game_mode();
    const int count = (int)choices.size();
    int sel = 0;
    for (int i = 1; i < count; i++) if (choices[i].id == last && choices[i].note.empty()) sel = i;
    auto draw = [&]() {
        recomp3ds::screen_printf("\x1b[8;0H%-40s", "Play:");
        int row = 10;
        for (int i = 0; i < count && row < 23; i++) {
            const Choice& c = choices[i];
            const char* colour = i == sel ? "\x1b[33m" : !c.note.empty() ? "\x1b[31m" : "";
            recomp3ds::screen_printf("\x1b[%d;0H%s%s %-37.37s\x1b[0m", row++, colour, i == sel ? ">" : " ", c.name.c_str());
            if (!c.note.empty() && row < 23) recomp3ds::screen_printf("\x1b[%d;0H\x1b[31m   %-37.37s\x1b[0m", row++, c.note.c_str());
        }
        recomp3ds::screen_printf("\x1b[24;0H%-40s", "D-Pad: choose   A: start");
    };
    auto step = [&](int dir) {
        // Only what can be played.
        for (int k = 1; k < count; k++) {
            int i = (sel + dir * k + count * k) % count;
            if (choices[i].note.empty()) { sel = i; return; }
        }
    };
    draw();
    u32 prev = 0;
    while (aptMainLoop()) {
        hidScanInput();
        const u32 held = hidKeysHeld() | recomp3ds::autotest_tick();
        const u32 down = held & ~prev;
        prev = held;
        if (down & KEY_A) break;
        if (down & KEY_DUP) { step(-1); draw(); }
        if (down & KEY_DDOWN) { step(+1); draw(); }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    for (int r = 8; r <= 24; r++) recomp3ds::screen_printf("\x1b[%d;0H%40s", r, "");
    fprintf(stderr, "recomp3ds: playing %s\n", sel == 0 ? "the game" : choices[sel].id.c_str());
    recomp3ds::settings_set_game_mode(choices[sel].id);
    return choices[sel].id;
}

namespace {
std::unordered_set<std::string> g_left_out;
}

void recomp3ds::game_mode_begin(const GameDesc& desc, const std::string& game_mode) {
    g_left_out.clear();
    if (game_mode.empty()) return;
    for (size_t i = 0; i < desc.mods_not_in_game_modes_count; i++) g_left_out.insert(desc.mods_not_in_game_modes[i]);
    recomp::mods::set_session_excluded_mods(g_left_out);
    for (const std::string& id : g_left_out) {
        if (recomp::mods::is_mod_enabled(id)) fprintf(stderr, "recomp3ds: %s stays off in game modes\n", id.c_str());
    }
}

bool recomp3ds::mod_left_out(const std::string& mod_id) { return g_left_out.contains(mod_id); }

#else

void recomp3ds::game_modes_init(const char*, const GameDesc&) {}
std::string recomp3ds::pick_game_mode(const GameDesc&) { return {}; }
void recomp3ds::game_mode_begin(const GameDesc&, const std::string&) {}
bool recomp3ds::mod_left_out(const std::string&) { return false; }

#endif
