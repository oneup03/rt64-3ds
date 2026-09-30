// Game modes (RT64_3DS_STATIC_MODS): mods that turn the game into another
// one, a ROM hack plus its recompiled code. The code is baked into the
// executable; the player brings the mod's .nrm, whose patch.bps makes the
// hack's ROM. Before the game starts a picker on the bottom screen offers the
// game and every game mode found; the runtime then hands the chosen mode's
// ROM patch to apply_rom_patch, which writes the patched ROM to the SD card
// once (it does not fit in memory: ROM hacks make 64 MB ROMs) and streams the
// ROM from it.
#include <3ds.h>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "recomp3ds.h"
#include "recomp3ds_internal.h"

#ifdef RECOMP_STATIC_MODS
#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "bps_stream.h"

namespace {

std::string g_base_path, g_rom_path;

void file_read(void* ctx, uint64_t offset, uint8_t* dst, size_t n) {
    FILE* f = static_cast<FILE*>(ctx);
    if (fseek(f, (long)offset, SEEK_SET) != 0 || fread(dst, 1, n, f) != n) memset(dst, 0, n);
}

void memory_read(void* ctx, uint64_t offset, uint8_t* dst, size_t n) {
    memcpy(dst, static_cast<const uint8_t*>(ctx) + offset, n);
}

void show_progress(void*, uint64_t done, uint64_t total) {
    printf("\x1b[12;0H  %3u%% of %u MB", (unsigned)(total ? done * 100 / total : 0), (unsigned)(total >> 20));
    gfxFlushBuffers();
}

// The patch the file was made with, next to it: a match skips reading the
// 64 MB back to check it on every start.
std::string stamp_of(std::span<const uint8_t> patch) {
    char s[48];
    const uint8_t* crc = patch.data() + patch.size() - 4;   // the patch's own CRC32, last in its footer
    snprintf(s, sizeof(s), "%02x%02x%02x%02x %llu", crc[3], crc[2], crc[1], crc[0], (unsigned long long)recomp3ds::bps_target_size(patch));
    return s;
}

bool stamp_matches(const std::string& rom, std::span<const uint8_t> patch) {
    struct stat st;
    if (stat(rom.c_str(), &st) != 0 || (uint64_t)st.st_size != recomp3ds::bps_target_size(patch)) return false;
    char line[64] = "";
    FILE* f = fopen((rom + ".ok").c_str(), "r");
    if (f == nullptr) return false;
    const bool read = fgets(line, sizeof(line), f) != nullptr;
    fclose(f);
    return read && stamp_of(patch) == std::string(line);
}

bool apply_rom_patch(const std::string& mod_id, std::span<const uint8_t> patch) {
    const std::string dir = g_base_path + "/modes";
    mkdir(dir.c_str(), 0777);
    const std::string rom = dir + "/" + mod_id + ".z64";
    if (!stamp_matches(rom, patch)) {
        FILE* src = fopen(g_rom_path.c_str(), "rb");
        if (src == nullptr) return false;
        fseek(src, 0, SEEK_END);
        const uint64_t size = (uint64_t)ftell(src);
        printf("\x1b[10;0HPreparing the game mode's ROM\x1b[11;0H(once; it takes a while):");
        fprintf(stderr, "recomp3ds: applying %s's ROM patch to %s\n", mod_id.c_str(), rom.c_str());
        const u64 t0 = svcGetSystemTick();
        // The source in memory if it fits (one read instead of thousands),
        // else read from the card as the patch needs it.
        std::vector<uint8_t> source;
        try { source.resize((size_t)size); } catch (const std::bad_alloc&) { source.clear(); }
        if (!source.empty()) {
            fseek(src, 0, SEEK_SET);
            if (fread(source.data(), 1, source.size(), src) != source.size()) source.clear();
        }
        const recomp3ds::BpsSource from = source.empty() ? recomp3ds::BpsSource{ size, file_read, src }
                                                         : recomp3ds::BpsSource{ size, memory_read, source.data() };
        const recomp3ds::BpsResult r = recomp3ds::bps_apply_to_file(patch, from, rom.c_str(), show_progress, nullptr);
        fclose(src);
        fprintf(stderr, "recomp3ds: ROM patch %s in %.1f s\n", recomp3ds::bps_result_string(r), (double)(svcGetSystemTick() - t0) / SYSCLOCK_ARM11);
        printf("\x1b[10;0H%40s\x1b[11;0H%40s\x1b[12;0H%40s", "", "", "");
        if (r != recomp3ds::BpsResult::Ok) {
            fprintf(stderr, "recomp3ds: %s's ROM patch: %s\n", mod_id.c_str(), recomp3ds::bps_result_string(r));
            return false;
        }
        if (FILE* f = fopen((rom + ".ok").c_str(), "w")) {
            fputs(stamp_of(patch).c_str(), f);
            fclose(f);
        }
    }
    // Read the patched ROM from the card; the one in memory (if any) goes.
    if (recomp::get_rom_stream() == nullptr) recomp3ds::rom_stream_install(2u << 20);
    if (!recomp::open_rom_stream(rom)) return false;
    recomp::set_rom_contents({});
    return true;
}

}   // namespace

void recomp3ds::game_modes_init(const char* base_path, const GameDesc& desc) {
    g_base_path = base_path;
    g_rom_path = std::string(base_path) + "/" + (const char*)desc.game_id.c_str() + ".z64";
    recomp::mods::set_rom_patch_handler(apply_rom_patch);
}

std::string recomp3ds::pick_game_mode(const GameDesc& desc) {
    // The runtime scans again when it starts; this early scan finds the modes.
    recomp::mods::initialize_mods();
    recomp::mods::scan_mods();
    std::vector<recomp::mods::ModDetails> modes;
    for (recomp::mods::ModDetails& d : recomp::mods::get_all_mod_details(desc.entry.mod_game_id)) {
        if (d.custom_gamemode) modes.push_back(std::move(d));
    }
    if (modes.empty()) return {};

    // 0 is the game itself; the last choice comes first.
    const std::string last = recomp::mods::get_latest_game_mode_id();
    int sel = 0;
    for (size_t i = 0; i < modes.size(); i++) if (modes[i].mod_id == last) sel = (int)i + 1;
    const int count = (int)modes.size() + 1;
    auto name = [&](int i) { return i == 0 ? desc.entry.display_name : modes[i - 1].display_name; };
    auto draw = [&]() {
        printf("\x1b[8;0H%-40s", "Play:");
        for (int i = 0; i < count && i < 12; i++) {
            printf("\x1b[%d;0H%s%s %-37.37s\x1b[0m", 10 + i, i == sel ? "\x1b[33m" : "", i == sel ? ">" : " ", name(i).c_str());
        }
        printf("\x1b[24;0H%-40s", "D-Pad: choose   A: start");
    };
    draw();
    u32 prev = 0;
    while (aptMainLoop()) {
        hidScanInput();
        const u32 held = hidKeysHeld() | recomp3ds::autotest_tick();
        const u32 down = held & ~prev;
        prev = held;
        if (down & KEY_A) break;
        if (down & KEY_DUP) { sel = (sel + count - 1) % count; draw(); }
        if (down & KEY_DDOWN) { sel = (sel + 1) % count; draw(); }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    for (int r = 8; r <= 24; r++) printf("\x1b[%d;0H%40s", r, "");
    fprintf(stderr, "recomp3ds: playing %s\n", sel == 0 ? "the game" : modes[sel - 1].mod_id.c_str());
    return sel == 0 ? std::string{} : modes[sel - 1].mod_id;
}

#else

void recomp3ds::game_modes_init(const char*, const GameDesc&) {}
std::string recomp3ds::pick_game_mode(const GameDesc&) { return {}; }

#endif
