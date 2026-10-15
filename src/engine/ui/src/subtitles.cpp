#include "mce/ui/subtitles.h"

#include <cmath>

#include "mce/convar.h"

namespace mce::ui {
namespace {

MCE_CVAR(cv_subtitles, "access.subtitles", true, "Show speech subtitles (timeline subtitle tracks, voiced lines)",
         CVarFlags::Saved);
MCE_CVAR(cv_captions, "access.captions", false, "Show sound captions (\"[door slams]\") for tagged sounds",
         CVarFlags::Saved);
MCE_CVAR(cv_subtitle_size, "access.subtitle_size", 1.0f, "Subtitle text size (0.5..3) on top of ui_scale and access.text_scale",
         CVarFlags::Saved);
MCE_CVAR(cv_subtitle_background, "access.subtitle_background", 0.6f,
         "Opacity of the box behind each subtitle line (0 = none: the text is outlined instead)", CVarFlags::Saved);
MCE_CVAR(cv_subtitle_speaker, "access.subtitle_speaker", true, "Name the speaker before each subtitle line",
         CVarFlags::Saved);

f32 to_linear(f32 c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
Color linear(Color c) { return Color{to_linear(c.r), to_linear(c.g), to_linear(c.b), c.a}; }
f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : v > hi ? hi : v; }

}  // namespace

SubtitleStyle subtitle_style() {
    SubtitleStyle s;
    s.speech = cv_subtitles.get_bool();
    s.sounds = cv_captions.get_bool();
    s.speaker_names = cv_subtitle_speaker.get_bool();
    s.size = clampf(cv_subtitle_size.get_float(), 0.5f, 3.0f);
    s.background = clampf(cv_subtitle_background.get_float(), 0.0f, 1.0f);
    return s;
}

void SubtitleView::update(Span<const SubtitleLine> lines, const UpdateContext& ctx, const SubtitleStyle& style) {
    count_ = 0;
    const render::Font* font = skin_.font;
    if (!font) return;
    const u32 max = style.max_lines < kMaxSubtitleLines ? style.max_lines : kMaxSubtitleLines;

    // The newest `max` lines the style shows, oldest first.
    u32 picked[kMaxSubtitleLines];
    u32 n = 0;
    for (usize i = lines.size(); i-- > 0 && n < max;) {
        const SubtitleLine& l = lines[i];
        if (l.text.empty() || (l.kind == SubtitleKind::Speech ? !style.speech : !style.sounds)) continue;
        picked[n++] = static_cast<u32>(i);
    }

    const f32 scale = ctx.ui_scale > 0.0f ? ctx.ui_scale : 1.0f;
    px_ = style.text_px * style.size * ctx.text_scale * scale;
    const f32 pad_x = 0.5f * px_, pad_y = 0.25f * px_, gap = 0.3f * px_;
    const f32 left = ctx.safe_insets.left, right = static_cast<f32>(ctx.viewport.width) - ctx.safe_insets.right;
    const f32 wrap = (right - left) * style.width - 2.0f * pad_x;
    f32 bottom = static_cast<f32>(ctx.viewport.height) - ctx.safe_insets.bottom - style.bottom * scale;

    Color fill{0, 0, 0, style.background};
    Color name = style.speaker, text = style.text, sound = style.sound;
    if (ctx.high_contrast) {
        fill.a = 1.0f;
        name = {1.0f, 0.9f, 0.0f, 1};
        text = sound = {1, 1, 1, 1};
    }

    render::TextLayoutDesc d;
    d.size_px = px_;
    d.max_width = wrap > px_ ? wrap : px_;
    d.align = render::TextAlign::Center;
    // Laid out newest first, from the bottom up.
    for (u32 k = 0; k < n; ++k) {
        const SubtitleLine& l = lines[picked[k]];
        Box& b = boxes_[n - 1 - k];
        b.shown.clear();
        b.name_glyphs = 0;
        const bool named = l.kind == SubtitleKind::Speech && style.speaker_names && !l.speaker.empty();
        if (named) {
            b.shown.assign(l.speaker);
            b.shown.append(":");
            render::TextLayoutDesc nd;
            nd.size_px = px_;
            render::layout_text(font->face, b.shown.view(), nd, part_);
            b.name_glyphs = static_cast<u32>(part_.quads.size());
            b.shown.append(" ");
        }
        const bool bracket = l.kind == SubtitleKind::Sound && !l.text.empty() && l.text[0] != '[';
        if (bracket) b.shown.append("[");
        b.shown.append(l.text);
        if (bracket) b.shown.append("]");
        render::layout_text(font->face, b.shown.view(), d, b.layout);  // glyphs in text order: the name's come first
        const f32 w = b.layout.width, h = b.layout.height;
        const f32 cx = 0.5f * (left + right);
        b.rect.min = vec2(std::floor(cx - 0.5f * w - pad_x), std::floor(bottom - h - 2.0f * pad_y));
        b.rect.max = vec2(std::ceil(cx + 0.5f * w + pad_x), std::ceil(bottom));
        b.text_at = vec2(cx - 0.5f * d.max_width, b.rect.min.y + pad_y);  // each line centred in the wrap width
        b.fill = fill;
        b.name = name;
        b.text = l.kind == SubtitleKind::Sound ? sound : text;
        b.outline = fill.a < 0.5f ? std::ceil(px_ / 16.0f) : 0.0f;  // no box to read against: outline the glyphs
        bottom = b.rect.min.y - gap;
    }
    count_ = n;
}

void SubtitleView::submit(render::SpriteBatch& batch, i16 sort_layer) const {
    const render::Font* font = skin_.font;
    if (!font) return;
    for (u32 i = 0; i < count_; ++i) {
        const Box& b = boxes_[i];
        if (b.fill.a > 0.0f) {
            render::RectQuad q;
            q.set = skin_.set;
            q.region = skin_.white_region;
            q.rect = math::aabb2{b.rect.min, b.rect.max};
            q.tint = linear(b.fill);
            q.sort_layer = sort_layer;
            q.sort_order = static_cast<i32>(i) * 4;
            batch.add_rect(q);
        }
        render::TextPlacement p;
        p.origin = vec2(std::floor(b.text_at.x), std::floor(b.text_at.y));
        p.sort_layer = sort_layer;
        p.sort_order = static_cast<i32>(i) * 4 + 1;
        p.outline_px = b.outline;
        p.outline_color = Color{0, 0, 0, 1};
        // The name's glyphs in the name's color, the rest in the line's.
        const usize split = b.name_glyphs < b.layout.quads.size() ? b.name_glyphs : b.layout.quads.size();
        if (split > 0) {
            part_.quads.clear();
            for (usize g = 0; g < split; ++g) part_.quads.push_back(b.layout.quads[g]);
            p.tint = linear(b.name);
            batch.add_text(*font, part_, p);
        }
        part_.quads.clear();
        for (usize g = split; g < b.layout.quads.size(); ++g) part_.quads.push_back(b.layout.quads[g]);
        p.tint = linear(b.text);
        p.sort_order = static_cast<i32>(i) * 4 + 2;
        batch.add_text(*font, part_, p);
    }
}

}  // namespace mce::ui
