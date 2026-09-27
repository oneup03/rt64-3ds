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
};

class Interpreter {
public:
    explicit Interpreter(uint8_t* rdram);

    // Runs a gfx task's display list into `out`. Returns true when a
    // G_RDPFULLSYNC was reached (a frame to present).
    bool run(uint32_t data_ptr, FrameRecord& out);
    const InterpreterStats& stats() const { return stats_; }

private:
    struct Impl;
    Impl* impl_;
    InterpreterStats stats_;
};

}   // namespace rt64_3ds

#endif
