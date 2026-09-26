// The 3DS entry point: console services, the runtime's callbacks, and the
// runtime itself. The game supplies a GameDesc; everything else is shared.
#include <3ds.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/stat.h>

#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/error_handling.hpp"
#include "ultramodern/events.hpp"
#include "ultramodern/host_thread.hpp"
#include "recomp3ds.h"
#include "recomp3ds_internal.h"

// libctru's default main stack is 32 KB; the runtime's start path and the
// C++ library want more.
extern "C" u32 __stacksize__ = 512 * 1024;

namespace {

char g_base_path[128] = "sdmc:/3ds";
const recomp3ds::GameDesc* g_desc = nullptr;
volatile bool g_exit_requested = false;

void* create_gfx() { return nullptr; }

ultramodern::renderer::WindowHandle create_window(void*) {
    return ultramodern::renderer::WindowHandle{};
}

// Runs on the main thread once a millisecond while the game is up.
void update_gfx(void*) {
    if (!aptMainLoop() || g_exit_requested) {
        static bool quitting = false;
        if (!quitting) {
            quitting = true;
            fprintf(stderr, "recomp3ds: quitting\n");
            ultramodern::quit();
        }
    }
}

void message_box(const char* msg) {
    fprintf(stderr, "[message] %s\n", msg);
    printf("\n%s\n", msg);
}

std::string game_thread_name(const OSThread* t) {
    if (g_desc != nullptr && g_desc->get_game_thread_name != nullptr) {
        return g_desc->get_game_thread_name(t);
    }
    return "Game Thread " + std::to_string(t->id);
}

std::unique_ptr<ultramodern::renderer::RendererContext>
make_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle handle, bool developer_mode) {
    if (g_desc != nullptr && g_desc->null_renderer) {
        return rt64_3ds::create_null_render_context(rdram, handle, developer_mode);
    }
    return rt64_3ds::create_render_context(rdram, handle, developer_mode);
}

void log_memory(const char* when) {
    fprintf(stderr, "recomp3ds: %s: app region free %lu KB, linear free %lu KB\n", when,
            (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) / 1024),
            (unsigned long)(linearSpaceFree() / 1024));
}

}   // namespace

const char* recomp3ds::base_path() { return g_base_path; }

void recomp3ds::request_exit() { g_exit_requested = true; }

int recomp3ds::run(const GameDesc& desc) {
    g_desc = &desc;

    gfxInitDefault();
    consoleInit(GFX_BOTTOM, nullptr);          // stdout: status text on the touch screen
    consoleDebugInit(debugDevice_SVC);         // stderr: the emulator / 3dslink log
    if (__3dslink_host.s_addr != 0) {
        link3dsStdio();
    }
    bool is_new_3ds = false;
    APT_CheckNew3DS(&is_new_3ds);
    fprintf(stderr, "recomp3ds: %s starting on %s 3DS\n", desc.render.game_name, is_new_3ds ? "a New" : "an Old");
    printf("%s\n", desc.render.game_name);
    log_memory("boot");

    snprintf(g_base_path, sizeof(g_base_path), "sdmc:/3ds/%s", desc.sd_dir);
    mkdir(g_base_path, 0777);
    {
        char sub[160];
        snprintf(sub, sizeof(sub), "%s/saves", g_base_path);
        mkdir(sub, 0777);
    }
    recomp::register_config_path(std::filesystem::path(g_base_path));

    recomp3ds::audio_init();
    rt64_3ds::set_render_desc(desc.render);

    // The runtime looks for <config>/<game_id>.z64; put the ROM there by that name.
    recomp::register_game(desc.entry);
    if (desc.register_overlays) {
        desc.register_overlays();
    }
    if (desc.register_patches) {
        desc.register_patches();
    }

    ultramodern::renderer::callbacks_t renderer_callbacks{};
    renderer_callbacks.create_render_context = make_render_context;

    ultramodern::audio_callbacks_t audio_callbacks{};
    audio_callbacks.queue_samples = recomp3ds::audio_queue_samples;
    audio_callbacks.get_frames_remaining = recomp3ds::audio_frames_remaining;
    audio_callbacks.set_frequency = recomp3ds::audio_set_frequency;

    ultramodern::input::callbacks_t input_callbacks{};
    input_callbacks.poll_input = recomp3ds::input_poll;
    input_callbacks.get_input = recomp3ds::input_get;
    input_callbacks.set_rumble = recomp3ds::input_set_rumble;
    input_callbacks.get_connected_device_info = recomp3ds::input_device_info;

    ultramodern::gfx_callbacks_t gfx_callbacks{};
    gfx_callbacks.create_gfx = create_gfx;
    gfx_callbacks.create_window = create_window;
    gfx_callbacks.update_gfx = update_gfx;

    ultramodern::events::callbacks_t events_callbacks{};
    ultramodern::error_handling::callbacks_t error_callbacks{};
    error_callbacks.message_box = message_box;
    ultramodern::threads::callbacks_t threads_callbacks{};
    threads_callbacks.get_game_thread_name = game_thread_name;

    recomp::Configuration cfg{};
    cfg.rsp_callbacks = desc.rsp;
    cfg.renderer_callbacks = renderer_callbacks;
    cfg.audio_callbacks = audio_callbacks;
    cfg.input_callbacks = input_callbacks;
    cfg.gfx_callbacks = gfx_callbacks;
    cfg.events_callbacks = events_callbacks;
    cfg.error_handling_callbacks = error_callbacks;
    cfg.threads_callbacks = threads_callbacks;
    cfg.message_queue_control.requeue_timer = false;

    if (!recomp::is_rom_valid(desc.game_id)) {
        printf("\nROM not found or wrong hash.\nPut it at %s/%s.z64\n\nPress START to exit.\n",
               g_base_path, (const char*)desc.game_id.c_str());
        fprintf(stderr, "recomp3ds: no valid ROM at %s/%s.z64\n", g_base_path, (const char*)desc.game_id.c_str());
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
        }
        gfxExit();
        return 1;
    }

    recomp::start_game(desc.game_id, "");
    log_memory("before start");
    recomp::start(cfg);        // returns when the game has quit
    log_memory("after exit");

    recomp3ds::audio_shutdown();
    gfxExit();
    return 0;
}
