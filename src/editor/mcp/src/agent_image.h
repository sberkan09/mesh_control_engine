#pragma once
// Small RGBA8 image helpers for the M-AGENT tools (render_frame, render_sheet, image_diff, the
// procedural generators): PPM/PNG files, base64 for MCP image content, blits, a tiny label font and
// the diff score. Private to mce_mcp_core. Rows top first, as read_pixels and PNG keep them.

#include "mce/cook/image.h"
#include "mce/cook/string.h"

namespace mce::mcp {

// Reads a binary PPM (P6, maxval 255) as written by mce_player -capture.
Result<cook::Image> read_ppm(StringView path);
Result<cook::Image> read_png_file(StringView path);
// PNG-encodes `img` into `out` (replacing it).
Result<void> write_png_file(StringView path, const cook::Image& img);

void base64_append(Span<const byte> bytes, cook::String& out);
// The PNG of `img`, base64-encoded (MCP image content).
Result<void> png_base64(const cook::Image& img, cook::String& out);

cook::Image make_image(u32 width, u32 height, u32 rgba);
// Copies `src` into `dst` at (x, y), clipped.
void blit(cook::Image& dst, const cook::Image& src, i32 x, i32 y);
void fill_rect(cook::Image& img, i32 x, i32 y, i32 w, i32 h, u32 rgba);
// Draws an outline `thickness` pixels wide (blended at `alpha`, 0..255).
void outline_rect(cook::Image& img, i32 x, i32 y, i32 w, i32 h, u32 rgba, u32 thickness, u32 alpha);
// A label in a 3x5 pixel font (digits, A-Z, a-z as capitals, space, '=', ':', '-', '.', '#', '/'),
// each pixel `scale` wide, on a dark box so it reads on any frame.
void draw_label(cook::Image& img, i32 x, i32 y, StringView text, u32 scale, u32 rgba);

struct DiffResult {
    f64 score = 0;             // mean absolute channel difference (RGB), 0..1
    f64 changed_fraction = 0;  // pixels with any RGB channel off by more than the threshold
    u32 max_channel_diff = 0;  // the largest RGB channel difference, 0..255
    cook::Image heatmap;       // the golden in grey with changed pixels in red (brighter = larger)
};
// `a` and `b` must have the same size.
DiffResult diff_images(const cook::Image& golden, const cook::Image& image, u32 threshold);

}  // namespace mce::mcp
