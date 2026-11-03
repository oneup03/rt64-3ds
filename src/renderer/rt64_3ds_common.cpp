// Settings, the per-game description, and the host-function hooks shared by
// every renderer implementation.
#include "rt64_3ds.h"

#include <3ds.h>
#include <atomic>
#include <cstdio>

namespace {
rt64_3ds::RenderDesc g_desc{};
rt64_3ds::Settings g_settings{};
rt64_3ds::FrameStats g_stats{};
volatile bool g_first_person = false;
volatile bool g_aim_textures = true;
std::atomic<u64> g_keep_frames_until{0};   // keep_frames' lease: its end, in system ticks
char g_data_dir[160] = "sdmc:/3ds";
}

void rt64_3ds::set_data_dir(const char* dir) { snprintf(g_data_dir, sizeof(g_data_dir), "%s", dir); }
void rt64_3ds::data_path(char* out, size_t size, const char* name) { snprintf(out, size, "%s/%s", g_data_dir, name); }
FILE* rt64_3ds::data_fopen(const char* name, const char* mode) {
    char path[224];
    data_path(path, sizeof(path), name);
    return fopen(path, mode);
}

void rt64_3ds::set_render_desc(const RenderDesc& desc) { g_desc = desc; }
const rt64_3ds::RenderDesc& rt64_3ds::render_desc() { return g_desc; }
rt64_3ds::ScreenGeometry rt64_3ds::screen_geometry() { return ScreenGeometry::make((int)g_desc.vi_width, (int)g_desc.vi_height, g_desc.display_aspect); }
rt64_3ds::Settings& rt64_3ds::settings() { return g_settings; }
const rt64_3ds::FrameStats& rt64_3ds::stats() { return g_stats; }
rt64_3ds::FrameStats& rt64_3ds::mutable_stats() { return g_stats; }
void rt64_3ds::set_first_person(bool on) { g_first_person = on; }
bool rt64_3ds::first_person_scene() { return g_first_person; }
void rt64_3ds::set_aim_textures(bool on) { g_aim_textures = on; }
bool rt64_3ds::aim_textures_on() { return g_aim_textures; }
void rt64_3ds::keep_frames(int ms) { g_keep_frames_until = svcGetSystemTick() + (u64)ms * (SYSCLOCK_ARM11 / 1000); }
bool rt64_3ds::keeping_frames() { return g_first_person || svcGetSystemTick() < g_keep_frames_until; }


