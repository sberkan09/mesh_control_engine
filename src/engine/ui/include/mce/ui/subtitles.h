#pragma once
// Subtitles and sound captions on screen (M-ACCESS step 1): a box per line, centred at the bottom
// of the safe area, newest at the bottom, the speaker's name in its own color before the line and
// sounds in brackets in a third. A view: the host gathers the lines (the stock player reads
// kit::caption_lines, mce/kit/captions.h: timeline subtitle tracks, SoundCaption tags and
// ShowCaption events) and draws them over the game, under the menus.
//
//   access.subtitles            1  speech lines (timeline subtitles, ShowCaption Speech)
//   access.captions             0  sound captions ("[door slams]")
//   access.subtitle_size        1  text size, 0.5..3, on top of ui_scale and access.text_scale
//   access.subtitle_background  0.6  opacity of the box behind each line (0 = text only, outlined)
//   access.subtitle_speaker     1  the speaker's name before the line
//
// access.high_contrast draws an opaque box with white text and a yellow name.

#include "mce/fixed_string.h"
#include "mce/render/sprite_batch.h"
#include "mce/render/text.h"
#include "mce/ui/ui.h"

namespace mce::ui {

enum class SubtitleKind : u8 { Speech, Sound };

struct SubtitleLine {
    StringView speaker;  // "" for narration and sounds
    StringView text;
    SubtitleKind kind = SubtitleKind::Speech;
};

struct SubtitleStyle {
    bool speech = true;          // access.subtitles
    bool sounds = false;         // access.captions
    bool speaker_names = true;   // access.subtitle_speaker
    f32 text_px = 22.0f;         // at UI scale 1, before size
    f32 size = 1.0f;             // access.subtitle_size
    f32 background = 0.6f;       // access.subtitle_background
    Color text{1, 1, 1, 1};
    Color speaker{1.0f, 0.82f, 0.35f, 1};
    Color sound{0.72f, 0.86f, 1.0f, 1};
    f32 width = 0.8f;            // of the safe area: lines wrap past it
    f32 bottom = 36.0f;          // pixels above the safe area's bottom at UI scale 1
    u32 max_lines = 3;           // the newest this many lines show (at most kMaxSubtitleLines)
};

inline constexpr u32 kMaxSubtitleLines = 8;

// The style the access.subtitle* / access.captions convars ask for.
SubtitleStyle subtitle_style();

class SubtitleView {
public:
    void set_skin(const Skin& skin) { skin_ = skin; }
    // Lays the lines out (oldest first, as kit::caption_lines gives them): the kinds the style
    // hides are dropped, then the newest max_lines show.
    void update(Span<const SubtitleLine> lines, const UpdateContext& ctx, const SubtitleStyle& style);
    void submit(render::SpriteBatch& batch, i16 sort_layer = 150) const;

    // What update() laid out (tests, the MCP preview): the line as drawn ("Ada: Run!", "[door
    // slams]") and its box in pixels.
    u32 shown() const { return count_; }
    StringView shown_text(u32 i) const { return boxes_[i].shown.view(); }
    Rect box(u32 i) const { return boxes_[i].rect; }

private:
    struct Box {
        FixedString<256> shown;
        u32 name_glyphs = 0;  // the first glyphs of the layout are the speaker's name
        Rect rect;
        vec2 text_at;
        Color fill, name, text;
        f32 outline = 0.0f;
        render::TextLayout layout;
    };
    Skin skin_;
    f32 px_ = 0.0f;
    u32 count_ = 0;
    Box boxes_[kMaxSubtitleLines];
    mutable render::TextLayout part_;
};

}  // namespace mce::ui
