// BPS patches whose output does not fit in memory: a ROM hack's 64 MB ROM on
// a console with 124 MB for everything. The output goes to a file, and the
// patch's target copies read back from it.
#ifndef RECOMP3DS_BPS_STREAM_H
#define RECOMP3DS_BPS_STREAM_H

#include <cstddef>
#include <cstdint>
#include <span>

namespace recomp3ds {

enum class BpsResult { Ok, BadPatch, WrongSource, BadTarget, IoError };
const char* bps_result_string(BpsResult r);

struct BpsSource {
    uint64_t size;
    // Reads source bytes [offset, offset + n).
    void (*read)(void* ctx, uint64_t offset, uint8_t* dst, size_t n);
    void* ctx;
};

// The target size the patch produces (0 for a malformed patch).
uint64_t bps_target_size(std::span<const uint8_t> patch);

// Whether `path` already holds the patch's target: its size and CRC32 match
// the patch's footer.
bool bps_target_matches(std::span<const uint8_t> patch, const char* path);

// Applies `patch` to `source`, writing the target to `path` (replaced once
// complete and verified). `progress`, if set, is told the bytes written so far.
BpsResult bps_apply_to_file(std::span<const uint8_t> patch, const BpsSource& source, const char* path,
                            void (*progress)(void* ctx, uint64_t done, uint64_t total) = nullptr, void* progress_ctx = nullptr);

}   // namespace recomp3ds

#endif
