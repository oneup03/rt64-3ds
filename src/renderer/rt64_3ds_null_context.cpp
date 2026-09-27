// A RendererContext that draws nothing: bring-up and CPU measurement. It
// still counts display lists per second, which is the game's frame rate.
#include <3ds.h>
#include <cstdio>

#include "rt64_3ds.h"

namespace {

class NullRenderContext final : public ultramodern::renderer::RendererContext {
public:
    NullRenderContext() {
        setup_result = ultramodern::renderer::SetupResult::Success;
    }
    bool valid() override { return true; }
    bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override { return true; }
    void enable_instant_present() override {}
    void send_dl(const OSTask* task) override {
        (void)task;
        if (dl_count == 0 && dl_per_sec == 0) fprintf(stderr, "rt64-3ds: first display list\n");
        dl_count++;
        u64 now = svcGetSystemTick();
        if (window_start == 0) {
            window_start = now;
        }
        else if (now - window_start >= SYSCLOCK_ARM11) {
            dl_per_sec = dl_count;
            rt64_3ds::mutable_stats().dl_per_sec = dl_count;
            dl_count = 0;
            window_start = now;
        }
    }
    // Not `override`: two of the runtime forks lack this virtual.
    void send_dummy_workload(uint32_t fb_address) { (void)fb_address; }
    void update_screen() override {}
    void shutdown() override {}
    uint32_t get_display_framerate() const override { return 60; }
    float get_resolution_scale() const override { return 1.0f; }

    int dl_per_sec = 0;
private:
    int dl_count = 0;
    u64 window_start = 0;
};

NullRenderContext* g_null = nullptr;

}   // namespace

std::unique_ptr<ultramodern::renderer::RendererContext>
rt64_3ds::create_null_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    (void)rdram; (void)window_handle; (void)developer_mode;
    auto ctx = std::make_unique<NullRenderContext>();
    g_null = ctx.get();
    fprintf(stderr, "rt64-3ds: null renderer (no drawing)\n");
    return ctx;
}

