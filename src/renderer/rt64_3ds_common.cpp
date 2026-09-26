// Settings, the per-game description, and the host-function hooks shared by
// every renderer implementation.
#include "rt64_3ds.h"

namespace {
rt64_3ds::RenderDesc g_desc{};
rt64_3ds::Settings g_settings{};
bool g_first_person = false;
bool g_low_convergence = false;
}

void rt64_3ds::set_render_desc(const RenderDesc& desc) { g_desc = desc; }
const rt64_3ds::RenderDesc& rt64_3ds::render_desc() { return g_desc; }
rt64_3ds::Settings& rt64_3ds::settings() { return g_settings; }
void rt64_3ds::set_first_person(bool on) { g_first_person = on; }
void rt64_3ds::set_low_convergence_scene(bool on) { g_low_convergence = on; }

void rt64_3ds::request_fb_readback(uint32_t fb_addr, int x, int y, int w, int h) {
    // Nothing to read back until there is a GPU renderer.
    (void)fb_addr; (void)x; (void)y; (void)w; (void)h;
}

std::unique_ptr<ultramodern::renderer::RendererContext>
rt64_3ds::create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    // Until the citro3d renderer exists, the real context is the null one.
    return create_null_render_context(rdram, window_handle, developer_mode);
}
