#include "mce/render/text.h"

namespace mce::render {

namespace {

struct LineRange {
    usize begin;
    usize end;  // exclusive, trailing break whitespace excluded
    f32 width;
};

struct Glyphs {
    const FontFace& font;
    const TextLayoutDesc& desc;
    f32 scale;

    // The glyph actually drawn for `cp` (fallback for missing ones), or null to skip.
    const FontGlyph* resolve(u32 cp) const {
        const FontGlyph* g = font.find(cp);
        if (!g && desc.fallback) g = font.find(desc.fallback);
        return g;
    }
    f32 advance(u32 prev, const FontGlyph* g) const {
        if (!g) return 0.0f;
        return (g->xadvance + (prev ? font.kern(prev, g->codepoint) : 0.0f)) * scale;
    }
};

// Finds the next line starting at byte `start`. Returns false when the text is exhausted.
bool next_line(const Glyphs& gl, StringView text, usize& start, LineRange& line) {
    if (start > text.size()) return false;
    if (start == text.size()) {
        // A trailing newline ends the text; it does not open an empty last line.
        return false;
    }
    const f32 max_w = gl.desc.max_width;
    f32 pen = 0.0f;
    u32 prev = 0;
    bool have_break = false;
    LineRange brk{};
    usize brk_next = 0;
    usize i = start;
    while (i < text.size()) {
        const usize at = i;
        const u32 cp = utf8_next(text, i);
        if (cp == '\n') {
            line = {start, at, pen};
            start = i;
            return true;
        }
        if (cp == ' ' || cp == '\t') {
            have_break = true;
            brk = {start, at, pen};
            brk_next = i;
        }
        const FontGlyph* g = gl.resolve(cp == '\t' ? ' ' : cp);
        const f32 adv = gl.advance(prev, g);
        if (max_w > 0.0f && pen + adv > max_w && cp != ' ' && cp != '\t' && have_break) {
            line = brk;
            // Swallow the run of spaces at the break.
            usize j = brk_next;
            while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) ++j;
            start = j;
            return true;
        }
        pen += adv;
        if (g) prev = g->codepoint;
    }
    line = {start, text.size(), pen};
    start = text.size() + 1;
    return true;
}

template <class Fn>
void for_each_line(const Glyphs& gl, StringView text, Fn&& fn) {
    usize start = 0;
    LineRange line{};
    u32 index = 0;
    while (next_line(gl, text, start, line)) fn(line, index++);
}

f32 scale_for(const FontFace& font, const TextLayoutDesc& desc) {
    const f32 baked = font.metrics().pixel_height;
    return desc.size_px > 0.0f && baked > 0.0f ? desc.size_px / baked : 1.0f;
}

}  // namespace

const FontGlyph* FontFace::find(u32 codepoint) const {
    usize lo = 0, hi = glyphs_.size();
    while (lo < hi) {
        const usize mid = (lo + hi) / 2;
        if (glyphs_[mid].codepoint < codepoint) lo = mid + 1;
        else hi = mid;
    }
    return lo < glyphs_.size() && glyphs_[lo].codepoint == codepoint ? &glyphs_[lo] : nullptr;
}

f32 FontFace::kern(u32 left, u32 right) const {
    usize lo = 0, hi = kerning_.size();
    while (lo < hi) {
        const usize mid = (lo + hi) / 2;
        const FontKern& k = kerning_[mid];
        if (k.left < left || (k.left == left && k.right < right)) lo = mid + 1;
        else hi = mid;
    }
    return lo < kerning_.size() && kerning_[lo].left == left && kerning_[lo].right == right ? kerning_[lo].advance : 0.0f;
}

u32 utf8_next(StringView s, usize& i) {
    constexpr u32 kReplacement = 0xFFFD;
    const auto b0 = static_cast<u8>(s[i++]);
    if (b0 < 0x80) return b0;
    u32 need = 0, cp = 0, min = 0;
    if ((b0 & 0xE0) == 0xC0) {
        need = 1;
        cp = b0 & 0x1Fu;
        min = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        need = 2;
        cp = b0 & 0x0Fu;
        min = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        need = 3;
        cp = b0 & 0x07u;
        min = 0x10000;
    } else {
        return kReplacement;
    }
    for (u32 k = 0; k < need; ++k) {
        if (i >= s.size()) return kReplacement;
        const auto b = static_cast<u8>(s[i]);
        if ((b & 0xC0) != 0x80) return kReplacement;  // do not consume: it may start the next codepoint
        cp = (cp << 6) | (b & 0x3Fu);
        ++i;
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return kReplacement;
    return cp;
}

math::vec2 measure_text(const FontFace& font, StringView text, const TextLayoutDesc& desc) {
    const Glyphs gl{font, desc, scale_for(font, desc)};
    f32 width = 0.0f;
    u32 lines = 0;
    for_each_line(gl, text, [&](const LineRange& l, u32) {
        width = math::max(width, l.width);
        ++lines;
    });
    return {width, static_cast<f32>(lines) * font.line_height() * gl.scale * desc.line_spacing};
}

void layout_text(const FontFace& font, StringView text, const TextLayoutDesc& desc, TextLayout& out) {
    out.quads.clear();
    out.width = out.height = 0.0f;
    out.lines = 0;
    if (font.empty()) return;
    const Glyphs gl{font, desc, scale_for(font, desc)};
    const f32 line_h = font.line_height() * gl.scale * desc.line_spacing;

    // Pass 1: line widths (alignment needs the widest line when there is no wrap width).
    f32 widest = 0.0f;
    for_each_line(gl, text, [&](const LineRange& l, u32) {
        widest = math::max(widest, l.width);
        ++out.lines;
    });
    out.width = widest;
    out.height = static_cast<f32>(out.lines) * line_h;
    const f32 box = desc.max_width > 0.0f ? desc.max_width : widest;

    const f32 inv_w = font.metrics().atlas_width ? 1.0f / static_cast<f32>(font.metrics().atlas_width) : 0.0f;
    const f32 inv_h = font.metrics().atlas_height ? 1.0f / static_cast<f32>(font.metrics().atlas_height) : 0.0f;
    const f32 s = gl.scale;

    // Pass 2: glyph quads.
    for_each_line(gl, text, [&](const LineRange& l, u32 index) {
        f32 pen = 0.0f;
        if (desc.align == TextAlign::Center) pen = 0.5f * (box - l.width);
        else if (desc.align == TextAlign::Right) pen = box - l.width;
        const f32 baseline = static_cast<f32>(index) * line_h + font.metrics().ascent * s;
        u32 prev = 0;
        for (usize i = l.begin; i < l.end;) {
            u32 cp = utf8_next(text, i);
            if (cp == '\t') cp = ' ';
            const FontGlyph* g = gl.resolve(cp);
            if (!g) continue;
            pen += prev ? font.kern(prev, g->codepoint) * s : 0.0f;
            if (g->x1 > g->x0 && g->y1 > g->y0) {
                GlyphQuad q;
                q.x0 = pen + g->xoff * s;
                q.y0 = baseline + g->yoff * s;
                q.x1 = pen + g->xoff2 * s;
                q.y1 = baseline + g->yoff2 * s;
                q.u0 = static_cast<f32>(g->x0) * inv_w;
                q.v0 = static_cast<f32>(g->y0) * inv_h;
                q.u1 = static_cast<f32>(g->x1) * inv_w;
                q.v1 = static_cast<f32>(g->y1) * inv_h;
                out.quads.push_back(q);
            }
            pen += g->xadvance * s;
            prev = g->codepoint;
        }
    });
}

}  // namespace mce::render
