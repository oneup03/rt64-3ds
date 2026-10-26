// Stand-ins for the desktop frontend's recompui API: see recompui_stubs.cpp.
#ifndef RECOMPUI_STUBS_H
#define RECOMPUI_STUBS_H

namespace recomp3ds {
    // Registers a do-nothing function, returning 0, under each recompui_*
    // export name, so a built-in mod that imports any of them still loads.
    // Call from main() before recomp3ds::run, as the game's own exports are.
    void register_recompui_stubs();
}

#endif
