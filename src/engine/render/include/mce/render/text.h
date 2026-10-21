#pragma once
// Text layout over cook-baked font atlases (FR-R2D-060, rendering-2d D9).
//
// The font atlas is baked at cook (stb_truetype coverage, or a signed distance field that stays
// sharp when magnified; MSDF at v0.5); the runtime never
// rasterizes. Layout is one function shared by HUD text, DebugDraw::text2d and the future
// mce::ui: UTF-8 decode -> glyph lookup -> advance + kerning -> word wrap -> alignment. It yields
// glyph quads in pixel space (+y down, origin at the top-left of the first line box); the
// renderer turns them into SpriteVertex quads through the normal batch.

#include "mce/containers/array.h"
#include "mce/math/math.h"
#include "mce/types.h"

namespace mce::render {

// Glyph and kerning records. Layout-identical to the cooked CHNK_FONT records (mce_asset
// FontGlyph / FontKern), so the asset adapter can hand the chunk's tables over without copying.
struct FontGlyph {  // sorted by codepoint
    u32 codepoint;
    u16 x0, y0, x1, y1;  // atlas rect in texels
    f32 xoff, yoff;      // quad top-left offset from the pen (y down)
    f32 xoff2, yoff2;    // quad bottom-right offset
    f32 xadvance;
};
static_assert(sizeof(FontGlyph) == 32);

struct FontKern {  // sorted by (left, right)
    u32 left;
    u32 right;
    f32 advance;
};
static_assert(sizeof(FontKern) == 12);

// A font as layout needs it: metrics at the baked pixel height + the tables. Views only; the
// owner (asset system or caller) keeps the bytes alive.
struct FontMetrics {
    f32 pixel_height = 0;  // the size the atlas was baked at
    f32 ascent = 0;        // pixels above the baseline (positive)
    f32 descent = 0;       // pixels below the baseline (negative)
    f32 line_gap = 0;
    u32 atlas_width = 0;
    u32 atlas_height = 0;
    f32 sdf_spread = 0;    // > 0: the atlas is a distance field (128 = edge) reaching this many pixels
};

class FontFace {
public:
    FontFace() = default;
    FontFace(const FontMetrics& m, Span<const FontGlyph> glyphs, Span<const FontKern> kerning)
        : metrics_(m), glyphs_(glyphs), kerning_(kerning) {}

    const FontMetrics& metrics() const { return metrics_; }
    const FontGlyph* find(u32 codepoint) const;  // binary search
    f32 kern(u32 left, u32 right) const;         // 0 when the pair has no entry
    f32 line_height() const { return metrics_.ascent - metrics_.descent + metrics_.line_gap; }
    bool empty() const { return glyphs_.empty(); }

private:
    FontMetrics metrics_;
    Span<const FontGlyph> glyphs_;
    Span<const FontKern> kerning_;
};

enum class TextAlign : u8 { Left, Center, Right };

struct TextLayoutDesc {
    f32 size_px = 0;          // target glyph height; 0 = the baked pixel height (crisp)
    f32 max_width = 0;        // wrap at whitespace past this width (pixels); 0 = no wrapping
    TextAlign align = TextAlign::Left;
    f32 line_spacing = 1.0f;  // multiplier on the font's line height
    f32 letter_spacing = 0;   // extra pixels between neighbouring glyphs (target size; may be negative)
    u32 fallback = '?';       // drawn for codepoints the font lacks (0 = skip them)
};

struct GlyphQuad {
    f32 x0, y0, x1, y1;  // pixel space, +y down
    f32 u0, v0, u1, v1;  // atlas uv, v0 = top
};

struct TextLayout {
    Array<GlyphQuad> quads;
    f32 width = 0;   // widest line, pixels
    f32 height = 0;  // line count × line height
    u32 lines = 0;

    explicit TextLayout(mem::IAllocator& a = mem::heap()) : quads(a) {}
};

// Decodes one UTF-8 codepoint at `text[i]`, advancing `i`. Malformed bytes yield U+FFFD.
u32 utf8_next(StringView text, usize& i);

// Lays `text` out; `out.quads` is cleared first. Newlines force breaks; with max_width, words
// wrap at spaces (a single over-long word stays on its own line rather than splitting).
void layout_text(const FontFace& font, StringView text, const TextLayoutDesc& desc, TextLayout& out);

// Measures without producing quads (same rules as layout_text).
math::vec2 measure_text(const FontFace& font, StringView text, const TextLayoutDesc& desc);

}  // namespace mce::render
