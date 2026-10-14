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

uint32_t kBlockBytes = 32 * 1024;      // set by rom_stream_install

struct Block {
    uint32_t index = UINT32_MAX;        // block number in the file
    uint32_t last_use = 0;
    uint8_t* data = nullptr;
    volatile bool loading = false;      // being read from the card (its data not yet valid)
};

FILE* g_file = nullptr;
uint64_t g_size = 0;
Block* g_blocks = nullptr;
uint32_t g_block_count = 0;
uint32_t g_clock = 0;
LightLock g_lock;                       // the blocks' bookkeeping
LightLock g_file_lock;                  // one card read at a time
LightEvent g_loaded;                    // a block's read completed
volatile uint32_t g_misses = 0, g_miss_bytes = 0;
// The game's waits in stream_read (a miss's card read, or a block the
// read-ahead thread is still reading): the calls over 50 us, their time and
// the longest, for the stats line; under g_lock.
volatile uint32_t g_waits = 0;
volatile uint64_t g_wait_ticks = 0, g_wait_max = 0;

// Read-ahead: a miss at block k also queues block k + 1 for a thread of its
// own, so a game that reads long runs (a map load, streamed music) finds the
// next block in the cache instead of waiting a card read for it. The card
// costs about 10 ms per read whatever its size.
Thread g_reader = nullptr;
LightEvent g_reader_wake;
volatile uint32_t g_reader_next = UINT32_MAX;
volatile bool g_reader_quit = false;

// g_lock held: the block holding `index`, or null.
Block* find_block(uint32_t index) {
    for (uint32_t i = 0; i < g_block_count; i++) if (g_blocks[i].index == index) return &g_blocks[i];
    return nullptr;
}

// g_lock held: the least recently used block that is not being read.
Block* choose_victim() {
    Block* victim = nullptr;
    for (uint32_t i = 0; i < g_block_count; i++) {
        Block& b = g_blocks[i];
        if (b.loading) continue;
        if (victim == nullptr || b.last_use < victim->last_use) victim = &b;
    }
    return victim;
}

// The card read itself, with no bookkeeping lock held.
void read_block(Block* b, uint32_t index) {
    const uint64_t offset = (uint64_t)index * kBlockBytes;
    size_t got = 0;
    LightLock_Lock(&g_file_lock);
    // The size under the file's lock: stream_open may have just changed it.
    const size_t want = offset >= g_size ? 0 : offset + kBlockBytes <= g_size ? kBlockBytes : (size_t)(g_size - offset);
    if (g_file != nullptr && want > 0 && fseek(g_file, (long)offset, SEEK_SET) == 0) got = fread(b->data, 1, want, g_file);
    LightLock_Unlock(&g_file_lock);
    if (got < kBlockBytes) memset(b->data + got, 0, kBlockBytes - got);
    g_misses = g_misses + 1;
    g_miss_bytes = g_miss_bytes + (uint32_t)got;
}

// g_lock held on entry and exit: the block holding `index`, read now if
// needed (the lock is dropped for the read, and while another thread's
// read of it finishes).
Block* block_for(uint32_t index) {
    for (;;) {
        Block* b = find_block(index);
        if (b != nullptr && !b->loading) { b->last_use = ++g_clock; return b; }
        if (b == nullptr) {
            Block* victim = choose_victim();
            if (victim == nullptr) {    // every block is being read: wait for one
                LightLock_Unlock(&g_lock);
                LightEvent_Wait(&g_loaded);
                LightLock_Lock(&g_lock);
                continue;
            }
            victim->index = index;
            victim->loading = true;
            victim->last_use = ++g_clock;
            LightLock_Unlock(&g_lock);
            read_block(victim, index);
            LightLock_Lock(&g_lock);
            victim->loading = false;
            LightEvent_Signal(&g_loaded);
            // The next block, read ahead while the game uses this one.
            if (g_reader != nullptr && (uint64_t)(index + 1) * kBlockBytes < g_size) {
                g_reader_next = index + 1;
                LightEvent_Signal(&g_reader_wake);
            }
            continue;
        }
        // Being read by the reader thread: wait for it.
        LightLock_Unlock(&g_lock);
        LightEvent_Wait(&g_loaded);
        LightLock_Lock(&g_lock);
    }
}

void reader_thread(void*) {
    while (!g_reader_quit) {
        LightEvent_Wait(&g_reader_wake);
        if (g_reader_quit) break;
        const uint32_t index = g_reader_next;
        g_reader_next = UINT32_MAX;
        if (index == UINT32_MAX) continue;
        LightLock_Lock(&g_lock);
        if (find_block(index) != nullptr) { LightLock_Unlock(&g_lock); continue; }
        Block* victim = choose_victim();
        if (victim == nullptr) { LightLock_Unlock(&g_lock); continue; }
        victim->index = index;
        victim->loading = true;
        victim->last_use = ++g_clock;
        LightLock_Unlock(&g_lock);
        read_block(victim, index);
        LightLock_Lock(&g_lock);
        victim->loading = false;
        LightEvent_Signal(&g_loaded);
        LightLock_Unlock(&g_lock);
    }
}

bool stream_open(const std::filesystem::path& path, uint64_t* size) {
    LightLock_Lock(&g_lock);
    // Another file (a game mode's ROM replacing the game's): what the cache
    // holds came from the old one.
    for (uint32_t i = 0; i < g_block_count; i++) g_blocks[i].index = UINT32_MAX;
    g_reader_next = UINT32_MAX;
    // The read-ahead thread may be reading the old file: the file changes
    // between two of its reads, never under one.
    LightLock_Lock(&g_file_lock);
    if (g_file != nullptr) fclose(g_file);
    g_file = fopen(path.string().c_str(), "rb");
    g_size = 0;
    if (g_file != nullptr) {
        setvbuf(g_file, nullptr, _IONBF, 0);
        fseek(g_file, 0, SEEK_END);
        g_size = (uint64_t)ftell(g_file);
    }
    LightLock_Unlock(&g_file_lock);
    LightLock_Unlock(&g_lock);
    if (g_file == nullptr) {
        fprintf(stderr, "recomp3ds: ROM stream: cannot open %s\n", path.string().c_str());
        return false;
    }
    *size = g_size;
    fprintf(stderr, "recomp3ds: ROM stream: %s, %llu KB, cache %lu x %lu KB\n", path.string().c_str(),
            (unsigned long long)(g_size / 1024), (unsigned long)g_block_count, (unsigned long)(kBlockBytes / 1024));
    return true;
}

void stream_read(uint64_t offset, uint8_t* dst, size_t size) {
    const u64 t0 = svcGetSystemTick();
    LightLock_Lock(&g_lock);
    while (size > 0) {
        if (offset >= g_size) { memset(dst, 0, size); break; }
        const Block* b = block_for((uint32_t)(offset / kBlockBytes));
        const uint32_t in_block = (uint32_t)(offset % kBlockBytes);
        const size_t n = size < kBlockBytes - in_block ? size : kBlockBytes - in_block;
        memcpy(dst, b->data + in_block, n);
        dst += n;
        offset += n;
        size -= n;
    }
    const u64 dt = svcGetSystemTick() - t0;
    if (dt > SYSCLOCK_ARM11 / 20000) {     // over 50 us: a card read waited for, not a cache hit
        g_waits = g_waits + 1;
        g_wait_ticks = g_wait_ticks + dt;
        if (dt > g_wait_max) g_wait_max = dt;
    }
    LightLock_Unlock(&g_lock);
}

const recomp::RomStream g_stream = { stream_open, stream_read };

}   // namespace

void recomp3ds::rom_stream_install(size_t cache_bytes, size_t block_bytes) {
    LightLock_Init(&g_lock);
    LightLock_Init(&g_file_lock);
    LightEvent_Init(&g_loaded, RESET_ONESHOT);
    LightEvent_Init(&g_reader_wake, RESET_ONESHOT);
    if (block_bytes >= 4096) kBlockBytes = (uint32_t)block_bytes;
    g_block_count = (uint32_t)(cache_bytes / kBlockBytes);
    if (g_block_count < 4) g_block_count = 4;
    g_blocks = new Block[g_block_count];
    for (uint32_t i = 0; i < g_block_count; i++) g_blocks[i].data = (uint8_t*)malloc(kBlockBytes);
    // The read-ahead thread, on the system core when the game may use it
    // (a New 3DS: core 3 is idle), else wherever the kernel puts it; it
    // waits on the card, so its priority hardly matters. Core 3 is asked
    // for on a New 3DS only: Azahar's Old 3DS hangs on a thread created
    // there (the hardware refuses it).
    bool is_new_3ds = false;
    APT_CheckNew3DS(&is_new_3ds);
    g_reader = is_new_3ds ? threadCreate(reader_thread, nullptr, 16 * 1024, 0x38, 3, false) : nullptr;
    if (g_reader == nullptr) g_reader = threadCreate(reader_thread, nullptr, 16 * 1024, 0x38, -2, false);
    fprintf(stderr, "recomp3ds: ROM stream: read-ahead thread %s\n", g_reader != nullptr ? "up" : "not created");
    recomp::set_rom_stream(&g_stream);
}

bool recomp3ds::rom_stream_active() {
    return g_file != nullptr;
}

void recomp3ds::rom_stream_take_stats(uint32_t* misses, uint32_t* bytes, uint32_t* waits, uint32_t* wait_ms, uint32_t* wait_max_ms) {
    *misses = g_misses;
    *bytes = g_miss_bytes;
    g_misses = 0;
    g_miss_bytes = 0;
    LightLock_Lock(&g_lock);
    *waits = g_waits;
    *wait_ms = (uint32_t)(g_wait_ticks * 1000ull / SYSCLOCK_ARM11);
    *wait_max_ms = (uint32_t)(g_wait_max * 1000ull / SYSCLOCK_ARM11);
    g_waits = 0;
    g_wait_ticks = 0;
    g_wait_max = 0;
    LightLock_Unlock(&g_lock);
}
