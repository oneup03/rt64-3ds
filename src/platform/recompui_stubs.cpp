// Stand-ins for the desktop frontend's recompui API (RecompFrontend's
// recompui::register_ui_exports), which the 3DS doesn't have. A mod that
// imports any of it, even only for an error popup it never shows (Banjo's
// German translation), otherwise fails to load: the runtime resolves every
// import as the mods load (CodeModLoadError::InvalidImport). Each name maps
// to one function that does nothing and returns 0, a null context or
// resource, which the API's callers take as failure.
#include "recompui_stubs.h"

#include "librecomp/overlays.hpp"
#include "recomp.h"

namespace {

void recompui_stub(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r2 = 0;
    ctx->r3 = 0;
    ctx->f0.d = 0.0;
}

// The names recompui::register_ui_exports registers on the desktop
// (RecompFrontend recompui/src/api).
const char* const kRecompuiExports[] = {
        "recompui_close_context",
        "recompui_context_root",
        "recompui_create_button",
        "recompui_create_context",
        "recompui_create_element",
        "recompui_create_imageview",
        "recompui_create_label",
        "recompui_create_labelradio",
        "recompui_create_passwordinput",
        "recompui_create_slider",
        "recompui_create_span",
        "recompui_create_style",
        "recompui_create_textinput",
        "recompui_create_texture_image_bytes",
        "recompui_create_texture_rgba32",
        "recompui_destroy_element",
        "recompui_destroy_texture",
        "recompui_get_input_text",
        "recompui_get_input_value_float",
        "recompui_get_input_value_u32",
        "recompui_hide_context",
        "recompui_open_context",
        "recompui_register_callback",
        "recompui_set_align_items",
        "recompui_set_background_color",
        "recompui_set_border_bottom_color",
        "recompui_set_border_bottom_left_radius",
        "recompui_set_border_bottom_right_radius",
        "recompui_set_border_bottom_width",
        "recompui_set_border_color",
        "recompui_set_border_left_color",
        "recompui_set_border_left_width",
        "recompui_set_border_radius",
        "recompui_set_border_right_color",
        "recompui_set_border_right_width",
        "recompui_set_border_top_color",
        "recompui_set_border_top_left_radius",
        "recompui_set_border_top_right_radius",
        "recompui_set_border_top_width",
        "recompui_set_border_width",
        "recompui_set_bottom",
        "recompui_set_color",
        "recompui_set_column_gap",
        "recompui_set_context_captures_input",
        "recompui_set_context_captures_mouse",
        "recompui_set_cursor",
        "recompui_set_display",
        "recompui_set_drag",
        "recompui_set_flex_basis",
        "recompui_set_flex_basis_auto",
        "recompui_set_flex_direction",
        "recompui_set_flex_grow",
        "recompui_set_flex_shrink",
        "recompui_set_font_size",
        "recompui_set_font_style",
        "recompui_set_font_weight",
        "recompui_set_gap",
        "recompui_set_height",
        "recompui_set_height_auto",
        "recompui_set_imageview_texture",
        "recompui_set_input_text",
        "recompui_set_input_value_float",
        "recompui_set_input_value_u32",
        "recompui_set_justify_content",
        "recompui_set_left",
        "recompui_set_letter_spacing",
        "recompui_set_line_height",
        "recompui_set_margin",
        "recompui_set_margin_auto",
        "recompui_set_margin_bottom",
        "recompui_set_margin_bottom_auto",
        "recompui_set_margin_left",
        "recompui_set_margin_left_auto",
        "recompui_set_margin_right",
        "recompui_set_margin_right_auto",
        "recompui_set_margin_top",
        "recompui_set_margin_top_auto",
        "recompui_set_max_height",
        "recompui_set_max_width",
        "recompui_set_min_height",
        "recompui_set_min_width",
        "recompui_set_nav",
        "recompui_set_nav_auto",
        "recompui_set_nav_none",
        "recompui_set_opacity",
        "recompui_set_overflow",
        "recompui_set_overflow_x",
        "recompui_set_overflow_y",
        "recompui_set_padding",
        "recompui_set_padding_bottom",
        "recompui_set_padding_left",
        "recompui_set_padding_right",
        "recompui_set_padding_top",
        "recompui_set_position",
        "recompui_set_right",
        "recompui_set_row_gap",
        "recompui_set_tab_index",
        "recompui_set_text",
        "recompui_set_text_align",
        "recompui_set_top",
        "recompui_set_visibility",
        "recompui_set_width",
        "recompui_set_width_auto",
        "recompui_show_context",
};

}  // namespace

void recomp3ds::register_recompui_stubs() {
    for (const char* name : kRecompuiExports) {
        recomp::overlays::register_base_export(name, recompui_stub);
    }
}
