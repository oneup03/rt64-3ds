// F3DEX2 (+ RT64 extended GBI) display-list interpreter producing a
// FrameRecord. CPU only; the vertex transform, lighting and RDP state
// tracking happen here so the backend only replays draws.
#ifndef RT64_3DS_DL_H
#define RT64_3DS_DL_H

#include <cstdint>

#include "rt64_3ds_record.h"

namespace rt64_3ds {

struct InterpreterStats {
    uint32_t commands = 0, tris = 0, rects = 0, unknown = 0, ex_unknown = 0;
    uint32_t dl_depth_overflow = 0;
    uint32_t tex_unresolved = 0;   // draws that wanted a texture no load covered
    uint32_t snapshots = 0;        // frames that copied the screen
    uint32_t op_hist[256] = {};    // commands run per opcode
    uint32_t mtx_loads = 0, vtx_loaded = 0, draws_merged = 0, probes = 0, clipped = 0;
};

class Interpreter {
public:
    explicit Interpreter(uint8_t* rdram);

    // Runs a gfx task's display list into `out`. Returns true when a
    // G_RDPFULLSYNC was reached (a frame to present).
    bool run(uint32_t data_ptr, FrameRecord& out);
    const InterpreterStats& stats() const { return stats_; }
    // The colour image a presented frame was drawn to: reads from it by a
    // later list are the game copying the screen (framebuffer effects).
    void note_presented_framebuffer(uint32_t addr);
    // Snapshot texture geometry the interpreter maps texel coordinates to.
    void set_snapshot_layout(int screen_x_offset);

private:
    struct Impl;
    Impl* impl_;
    InterpreterStats stats_;
};

}   // namespace rt64_3ds

#endif
