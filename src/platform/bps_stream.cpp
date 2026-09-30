// BPS patching to a file (bps_stream.h). The format:
// https://github.com/blakesmith/rombp/blob/master/docs/bps_spec.md
#include "bps_stream.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// CRC32, eight bytes a step (slicing-by-8): the patch checks 80 MB of it,
// which a byte at a time costs the ARM11 tens of seconds.
uint32_t g_crc_table[8][256];

void init_crc_table() {
    if (g_crc_table[0][1] != 0) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++) {
        for (int t = 1; t < 8; t++) g_crc_table[t][i] = (g_crc_table[t - 1][i] >> 8) ^ g_crc_table[0][g_crc_table[t - 1][i] & 0xFF];
    }
}

// Running CRC32: start at 0xFFFFFFFF, finish with ^ 0xFFFFFFFF.
uint32_t crc_update(uint32_t crc, const uint8_t* p, size_t n) {
    while (n >= 8) {
        const uint32_t a = crc ^ ((uint32_t)p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
        const uint32_t b = (uint32_t)p[4] | (p[5] << 8) | (p[6] << 16) | ((uint32_t)p[7] << 24);
        crc = g_crc_table[7][a & 0xFF] ^ g_crc_table[6][(a >> 8) & 0xFF] ^ g_crc_table[5][(a >> 16) & 0xFF] ^ g_crc_table[4][a >> 24] ^
              g_crc_table[3][b & 0xFF] ^ g_crc_table[2][(b >> 8) & 0xFF] ^ g_crc_table[1][(b >> 16) & 0xFF] ^ g_crc_table[0][b >> 24];
        p += 8; n -= 8;
    }
    for (size_t i = 0; i < n; i++) crc = (crc >> 8) ^ g_crc_table[0][(crc ^ p[i]) & 0xFF];
    return crc;
}

uint32_t read_le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

struct Header {
    uint64_t source_size = 0, target_size = 0;
    size_t actions = 0, actions_end = 0;      // offsets in the patch
    uint32_t source_crc = 0, target_crc = 0;
};

bool read_number(std::span<const uint8_t> patch, size_t& off, size_t end, uint64_t& out) {
    uint64_t data = 0, shift = 1;
    while (true) {
        if (off >= end || shift > (1ull << 56)) return false;
        const uint8_t x = patch[off++];
        data += (uint64_t)(x & 0x7F) * shift;
        if (x & 0x80) break;
        shift <<= 7;
        data += shift;
    }
    out = data;
    return true;
}

bool read_signed(std::span<const uint8_t> patch, size_t& off, size_t end, int64_t& out) {
    uint64_t raw;
    if (!read_number(patch, off, end, raw)) return false;
    out = (raw & 1) ? -(int64_t)(raw >> 1) : (int64_t)(raw >> 1);
    return true;
}

bool parse_header(std::span<const uint8_t> patch, Header& h) {
    if (patch.size() < 4 + 3 + 12 || memcmp(patch.data(), "BPS1", 4) != 0) return false;
    const size_t end = patch.size() - 12;     // the footer: source, target and patch CRC32s
    size_t off = 4;
    uint64_t metadata = 0;
    if (!read_number(patch, off, end, h.source_size) || !read_number(patch, off, end, h.target_size) ||
        !read_number(patch, off, end, metadata) || metadata > end - off) {
        return false;
    }
    h.actions = off + (size_t)metadata;
    h.actions_end = end;
    h.source_crc = read_le32(&patch[end]);
    h.target_crc = read_le32(&patch[end + 4]);
    return true;
}

// The output: appended through a buffer, and read back (target copies) from
// the buffer or, through a cache of blocks, from the file. A hack's patch
// copies from all over its output (tens of thousands of copies), with
// locality: small blocks, the least recently used replaced.
class Output {
    static constexpr size_t kBlock = 32u << 10, kBlocks = 32;
public:
    explicit Output(FILE* f) : f_(f), buf_(1u << 20), cache_(kBlock * kBlocks) {}

    uint64_t size() const { return flushed_ + buf_len_; }
    uint32_t crc() const { return crc_ ^ 0xFFFFFFFFu; }
    bool ok() const { return ok_; }

    void append(const uint8_t* p, size_t n) {
        while (n > 0 && ok_) {
            const size_t k = std::min(n, buf_.size() - buf_len_);
            memcpy(&buf_[buf_len_], p, k);
            crc_ = crc_update(crc_, p, k);
            buf_len_ += k; p += k; n -= k;
            if (buf_len_ == buf_.size()) flush();
        }
    }

    // Reads output bytes [pos, pos + n), all already appended.
    void read(uint64_t pos, uint8_t* dst, size_t n) {
        while (n > 0 && ok_) {
            size_t k;
            if (pos >= flushed_) {
                k = n;
                memcpy(dst, &buf_[pos - flushed_], k);
            }
            else {
                const uint64_t block = pos / kBlock;
                const size_t slot = slot_for(block);
                if (!ok_) return;
                const uint64_t block_end = std::min<uint64_t>((block + 1) * kBlock, flushed_);
                k = (size_t)std::min<uint64_t>(n, block_end - pos);
                memcpy(dst, &cache_[slot * kBlock + (pos - block * kBlock)], k);
            }
            pos += k; dst += k; n -= k;
        }
    }

    void flush() {
        if (buf_len_ == 0 || !ok_) return;
        if (fseek(f_, (long)flushed_, SEEK_SET) != 0 || fwrite(buf_.data(), 1, buf_len_, f_) != buf_len_) ok_ = false;
        // The last block may have been cached short: it grows now.
        for (size_t i = 0; i < kBlocks; i++) if (tags_[i] != ~0ull && (tags_[i] + 1) * kBlock > flushed_) tags_[i] = ~0ull;
        flushed_ += buf_len_;
        buf_len_ = 0;
    }

private:
    // The cache slot holding output block `block` (below flushed_), read in if needed.
    size_t slot_for(uint64_t block) {
        size_t victim = 0;
        for (size_t i = 0; i < kBlocks; i++) {
            if (tags_[i] == block) { uses_[i] = ++clock_; return i; }
            if (uses_[i] < uses_[victim]) victim = i;
        }
        const uint64_t start = block * kBlock;
        const size_t want = (size_t)std::min<uint64_t>(kBlock, flushed_ - start);
        if (fflush(f_) != 0 || fseek(f_, (long)start, SEEK_SET) != 0 || fread(&cache_[victim * kBlock], 1, want, f_) != want) {
            ok_ = false;
            return victim;
        }
        tags_[victim] = block;
        uses_[victim] = ++clock_;
        return victim;
    }

    FILE* f_;
    std::vector<uint8_t> buf_, cache_;
    uint64_t tags_[kBlocks] = { ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull,
                                ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull };
    uint64_t uses_[kBlocks] = {}, clock_ = 0;
    size_t buf_len_ = 0;
    uint64_t flushed_ = 0;
    uint32_t crc_ = 0xFFFFFFFFu;
    bool ok_ = true;
};

}   // namespace

const char* recomp3ds::bps_result_string(BpsResult r) {
    switch (r) {
        case BpsResult::Ok: return "ok";
        case BpsResult::BadPatch: return "the patch is damaged";
        case BpsResult::WrongSource: return "the ROM is not the one the patch is for";
        case BpsResult::BadTarget: return "the patched ROM failed its check";
        case BpsResult::IoError: return "the SD card could not be written";
    }
    return "?";
}

uint64_t recomp3ds::bps_target_size(std::span<const uint8_t> patch) {
    Header h;
    return parse_header(patch, h) ? h.target_size : 0;
}

bool recomp3ds::bps_target_matches(std::span<const uint8_t> patch, const char* path) {
    init_crc_table();
    Header h;
    if (!parse_header(patch, h)) return false;
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return false;
    std::vector<uint8_t> buf(256u << 10);
    uint64_t total = 0;
    uint32_t crc = 0xFFFFFFFFu;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) {
        crc = crc_update(crc, buf.data(), n);
        total += n;
    }
    fclose(f);
    return total == h.target_size && (crc ^ 0xFFFFFFFFu) == h.target_crc;
}

recomp3ds::BpsResult recomp3ds::bps_apply_to_file(std::span<const uint8_t> patch, const BpsSource& source, const char* path,
                                                  void (*progress)(void* ctx, uint64_t done, uint64_t total), void* progress_ctx) {
    init_crc_table();
    Header h;
    if (!parse_header(patch, h)) return BpsResult::BadPatch;

    // The source must be the patch's: its size and CRC32.
    if (source.size != h.source_size) return BpsResult::WrongSource;
    std::vector<uint8_t> chunk(256u << 10);
    {
        uint32_t crc = 0xFFFFFFFFu;
        for (uint64_t pos = 0; pos < source.size; pos += chunk.size()) {
            const size_t n = (size_t)std::min<uint64_t>(chunk.size(), source.size - pos);
            source.read(source.ctx, pos, chunk.data(), n);
            crc = crc_update(crc, chunk.data(), n);
        }
        if ((crc ^ 0xFFFFFFFFu) != h.source_crc) return BpsResult::WrongSource;
    }

    const std::string tmp = std::string(path) + ".part";
    FILE* f = fopen(tmp.c_str(), "w+b");
    if (f == nullptr) return BpsResult::IoError;
    Output out(f);
    BpsResult result = BpsResult::Ok;
    size_t off = h.actions;
    uint64_t source_rel = 0, target_rel = 0, next_report = 0;

    // Copies source bytes [from, from + n) to the output.
    auto copy_source = [&](uint64_t from, uint64_t n) {
        if (from + n > source.size) return false;
        while (n > 0) {
            const size_t k = (size_t)std::min<uint64_t>(n, chunk.size());
            source.read(source.ctx, from, chunk.data(), k);
            out.append(chunk.data(), k);
            from += k; n -= k;
        }
        return true;
    };

    while (off < h.actions_end && out.ok()) {
        uint64_t data;
        if (!read_number(patch, off, h.actions_end, data)) { result = BpsResult::BadPatch; break; }
        const uint64_t length = (data >> 2) + 1;
        if (out.size() + length > h.target_size) { result = BpsResult::BadPatch; break; }
        switch (data & 3) {
            case 0:     // SourceRead: the source at the output's position
                if (!copy_source(out.size(), length)) result = BpsResult::BadPatch;
                break;
            case 1:     // TargetRead: bytes from the patch
                if (length > h.actions_end - off) { result = BpsResult::BadPatch; break; }
                out.append(&patch[off], (size_t)length);
                off += (size_t)length;
                break;
            case 2: {   // SourceCopy: anywhere in the source
                int64_t delta;
                if (!read_signed(patch, off, h.actions_end, delta) || (int64_t)source_rel + delta < 0) { result = BpsResult::BadPatch; break; }
                source_rel += delta;
                if (!copy_source(source_rel, length)) result = BpsResult::BadPatch;
                source_rel += length;
                break;
            }
            case 3: {   // TargetCopy: earlier output, possibly overlapping what it writes
                int64_t delta;
                if (!read_signed(patch, off, h.actions_end, delta) || (int64_t)target_rel + delta < 0) { result = BpsResult::BadPatch; break; }
                target_rel += delta;
                uint64_t left = length;
                while (left > 0 && out.ok()) {
                    if (target_rel >= out.size()) { result = BpsResult::BadPatch; break; }
                    const uint64_t avail = out.size() - target_rel;
                    size_t k;
                    if (avail < left && avail < chunk.size()) {
                        // Overlapping its own output: the last `avail` bytes repeat
                        // (a ROM hack's padding is a 1-byte period over megabytes).
                        const size_t d = (size_t)avail;
                        out.read(target_rel, chunk.data(), d);
                        k = (size_t)std::min<uint64_t>(left, chunk.size());
                        // Doubling copies: [0, d) -> [d, 2d) -> [2d, 4d) ...
                        for (size_t have = d; have < k; have *= 2) memcpy(&chunk[have], chunk.data(), std::min(have, k - have));
                    }
                    else {
                        k = (size_t)std::min<uint64_t>({ left, avail, chunk.size() });
                        out.read(target_rel, chunk.data(), k);
                    }
                    out.append(chunk.data(), k);
                    target_rel += k; left -= k;
                }
                break;
            }
        }
        if (result != BpsResult::Ok) break;
        if (progress != nullptr && out.size() >= next_report) {
            progress(progress_ctx, out.size(), h.target_size);
            next_report = out.size() + (1u << 20);
        }
    }
    out.flush();
    const bool io_ok = out.ok();
    if (fclose(f) != 0 && result == BpsResult::Ok) result = BpsResult::IoError;
    if (result == BpsResult::Ok && !io_ok) result = BpsResult::IoError;
    if (result == BpsResult::Ok && (out.size() != h.target_size || out.crc() != h.target_crc)) result = BpsResult::BadTarget;
    if (result == BpsResult::Ok) {
        remove(path);
        if (rename(tmp.c_str(), path) != 0) result = BpsResult::IoError;
    }
    if (result != BpsResult::Ok) remove(tmp.c_str());
    if (progress != nullptr && result == BpsResult::Ok) progress(progress_ctx, h.target_size, h.target_size);
    return result;
}
