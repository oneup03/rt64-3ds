// The ROM read from the SD card as the game needs it (recomp::RomStream),
// for when memory cannot hold it whole: an Old 3DS's 64 MB against DK64's
// 32 MB ROM beside RDRAM and everything else. Reads go through a small cache
// of fixed-size blocks, the least recently used replaced; a miss reads one
// block from the file (stdio unbuffered, so one FS read each).
#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "librecomp/game.hpp"
#include "recomp3ds_internal.h"

namespace {

constexpr uint32_t kBlockBytes = 32 * 1024;

struct Block {
    uint32_t index = UINT32_MAX;        // block number in the file
    uint32_t last_use = 0;
    uint8_t* data = nullptr;
};

FILE* g_file = nullptr;
uint64_t g_size = 0;
Block* g_blocks = nullptr;
uint32_t g_block_count = 0;
uint32_t g_clock = 0;
LightLock g_lock;
volatile uint32_t g_misses = 0, g_miss_bytes = 0;

const Block& block_for(uint32_t index) {
    Block* victim = &g_blocks[0];
    for (uint32_t i = 0; i < g_block_count; i++) {
        Block& b = g_blocks[i];
        if (b.index == index) { b.last_use = ++g_clock; return b; }
        if (b.last_use < victim->last_use) victim = &b;
    }
    const uint64_t offset = (uint64_t)index * kBlockBytes;
    const size_t want = offset + kBlockBytes <= g_size ? kBlockBytes : (size_t)(g_size - offset);
    size_t got = 0;
    if (fseek(g_file, (long)offset, SEEK_SET) == 0) got = fread(victim->data, 1, want, g_file);
    if (got < kBlockBytes) memset(victim->data + got, 0, kBlockBytes - got);
    victim->index = index;
    victim->last_use = ++g_clock;
    g_misses = g_misses + 1;
    g_miss_bytes = g_miss_bytes + (uint32_t)got;
    return *victim;
}

bool stream_open(const std::filesystem::path& path, uint64_t* size) {
    if (g_file != nullptr) fclose(g_file);
    g_file = fopen(path.string().c_str(), "rb");
    if (g_file == nullptr) {
        fprintf(stderr, "recomp3ds: ROM stream: cannot open %s\n", path.string().c_str());
        return false;
    }
    setvbuf(g_file, nullptr, _IONBF, 0);
    fseek(g_file, 0, SEEK_END);
    g_size = (uint64_t)ftell(g_file);
    *size = g_size;
    fprintf(stderr, "recomp3ds: ROM stream: %s, %llu KB, cache %lu x %lu KB\n", path.string().c_str(),
            (unsigned long long)(g_size / 1024), (unsigned long)g_block_count, (unsigned long)(kBlockBytes / 1024));
    return true;
}

void stream_read(uint64_t offset, uint8_t* dst, size_t size) {
    LightLock_Lock(&g_lock);
    while (size > 0) {
        if (offset >= g_size) { memset(dst, 0, size); break; }
        const Block& b = block_for((uint32_t)(offset / kBlockBytes));
        const uint32_t in_block = (uint32_t)(offset % kBlockBytes);
        const size_t n = size < kBlockBytes - in_block ? size : kBlockBytes - in_block;
        memcpy(dst, b.data + in_block, n);
        dst += n;
        offset += n;
        size -= n;
    }
    LightLock_Unlock(&g_lock);
}

const recomp::RomStream g_stream = { stream_open, stream_read };

}   // namespace

void recomp3ds::rom_stream_install(size_t cache_bytes) {
    LightLock_Init(&g_lock);
    g_block_count = (uint32_t)(cache_bytes / kBlockBytes);
    if (g_block_count < 4) g_block_count = 4;
    g_blocks = new Block[g_block_count];
    for (uint32_t i = 0; i < g_block_count; i++) g_blocks[i].data = (uint8_t*)malloc(kBlockBytes);
    recomp::set_rom_stream(&g_stream);
}

bool recomp3ds::rom_stream_active() {
    return g_file != nullptr;
}

void recomp3ds::rom_stream_take_stats(uint32_t* misses, uint32_t* bytes) {
    *misses = g_misses;
    *bytes = g_miss_bytes;
    g_misses = 0;
    g_miss_bytes = 0;
}
