// Built-in mods whose binary the game's ROM carries (RT64_3DS_STATIC_MODS,
// mod_bake's :rom): a mod too big for the console beside the game, whose
// data a script folds into the ROM on a PC (Dinosaur Planet's Dinomod
// Enhanced: its 40 MB of asset files become the ROM's own, and its remaining
// 1 MB of data goes after them). mod_bake builds the mod in without its
// mod_binary.bin, and the runtime asks this file for it
// (recomp::mods::set_mod_binary_source, nmr 0035), streaming it into RDRAM
// and checking its hash against the one baked.
//
// The ROM says where: its last 16 bytes are "RT64MODS", the table's offset
// and its entry count (little-endian); each entry is the mod's id (48 bytes,
// NUL-padded), its binary's offset and size in the ROM file and the binary's
// FNV-1a 64-bit hash.
#include <3ds.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "recomp3ds_internal.h"

#ifdef RECOMP_STATIC_MODS
#include "librecomp/mods.hpp"

namespace {

struct Entry {
    std::string id;
    uint32_t offset;
    uint32_t size;
};

std::string g_rom_path;
std::vector<Entry> g_entries;
bool g_table_read = false;

void read_table() {
    g_table_read = true;
    FILE* f = fopen(g_rom_path.c_str(), "rb");
    if (f == nullptr) return;
    fseek(f, 0, SEEK_END);
    const long file_size = ftell(f);
    struct { char magic[8]; uint32_t table; uint32_t count; } footer{};
    if (file_size < 16 || fseek(f, file_size - 16, SEEK_SET) != 0 || fread(&footer, 1, 16, f) != 16 || memcmp(footer.magic, "RT64MODS", 8) != 0) {
        fclose(f);
        return;
    }
    if (footer.count > 16 || (uint64_t)footer.table + (uint64_t)footer.count * 64 > (uint64_t)file_size || fseek(f, footer.table, SEEK_SET) != 0) {
        fprintf(stderr, "recomp3ds: the ROM's table of mods is damaged\n");
        fclose(f);
        return;
    }
    for (uint32_t i = 0; i < footer.count; i++) {
        struct { char id[48]; uint32_t offset; uint32_t size; uint64_t hash; } e{};
        if (fread(&e, 1, sizeof(e), f) != sizeof(e)) break;
        e.id[47] = '\0';
        if ((uint64_t)e.offset + e.size > (uint64_t)file_size) {
            fprintf(stderr, "recomp3ds: the ROM's entry for %s runs past its end\n", e.id);
            continue;
        }
        g_entries.push_back(Entry{ e.id, e.offset, e.size });
        fprintf(stderr, "recomp3ds: the ROM carries %s's data (%lu KB at 0x%lX)\n", e.id, (unsigned long)(e.size >> 10), (unsigned long)e.offset);
    }
    fclose(f);
}

bool binary_source(const std::string& mod_id, recomp::mods::ModFileHandle::Sink sink, void* opaque, bool& exists, uint64_t& size) {
    if (!g_table_read) read_table();
    exists = false;
    size = 0;
    const Entry* e = nullptr;
    for (const Entry& x : g_entries) if (x.id == mod_id) e = &x;
    if (e == nullptr) return true;
    exists = true;
    size = e->size;
    if (sink == nullptr) return true;
    FILE* f = fopen(g_rom_path.c_str(), "rb");
    if (f == nullptr || fseek(f, e->offset, SEEK_SET) != 0) {
        if (f != nullptr) fclose(f);
        return false;
    }
    // In 128 KB reads: the card costs ~10 ms a read whatever its size.
    constexpr size_t kChunk = 128 * 1024;
    uint8_t* buf = static_cast<uint8_t*>(malloc(kChunk));
    bool ok = buf != nullptr;
    for (uint64_t done = 0; ok && done < e->size;) {
        const size_t n = (size_t)std::min<uint64_t>(kChunk, e->size - done);
        if (fread(buf, 1, n, f) != n) { ok = false; break; }
        if (!sink(opaque, done, buf, n)) break;   // the reader has what it wants
        done += n;
    }
    free(buf);
    fclose(f);
    if (!ok) fprintf(stderr, "recomp3ds: reading %s's data from the ROM failed\n", mod_id.c_str());
    return ok;
}

}   // namespace

void recomp3ds::rom_mods_install(const char* rom_path) {
    g_rom_path = rom_path;
    recomp::mods::set_mod_binary_source(binary_source);
}

#else

void recomp3ds::rom_mods_install(const char*) {}

#endif
