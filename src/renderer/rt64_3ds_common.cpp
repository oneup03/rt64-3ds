// Settings, the per-game description, and the host-function hooks shared by
// every renderer implementation.
#include "rt64_3ds.h"

namespace {
rt64_3ds::RenderDesc g_desc{};
rt64_3ds::Settings g_settings{};
rt64_3ds::FrameStats g_stats{};
volatile bool g_first_person = false;
volatile bool g_low_convergence = false;
}

void rt64_3ds::set_render_desc(const RenderDesc& desc) { g_desc = desc; }
const rt64_3ds::RenderDesc& rt64_3ds::render_desc() { return g_desc; }
rt64_3ds::Settings& rt64_3ds::settings() { return g_settings; }
const rt64_3ds::FrameStats& rt64_3ds::stats() { return g_stats; }
rt64_3ds::FrameStats& rt64_3ds::mutable_stats() { return g_stats; }
void rt64_3ds::set_first_person(bool on) { g_first_person = on; }
void rt64_3ds::set_low_convergence_scene(bool on) { g_low_convergence = on; }
bool rt64_3ds::first_person_scene() { return g_first_person; }
bool rt64_3ds::low_convergence_scene() { return g_low_convergence; }


