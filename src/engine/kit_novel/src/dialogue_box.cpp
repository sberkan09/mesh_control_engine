// The default NPC dialogue box (novel_ui.h): a non-modal mce_ui document over the conversation the
// sim runs. Everything here is a view; the sim's DialogueInput owns the input.
#include "mce/kit/novel_ui.h"

#include "mce/kit/novel.h"
#include "mce/ui/subtitles.h"

namespace mce::novel {

namespace {

ui::Style flat(math::Color tint, math::Color text, f32 px, bool quad) {
    ui::Style s;
    s.tint = tint;
    s.text_color = text;
    s.text_size = px;
    s.draw_quad = quad;
    return s;
}

ui::Anchors band(ui::vec2 min, ui::vec2 max, ui::vec2 off_min, ui::vec2 off_max) {
    ui::Anchors a;
    a.min = min;
    a.max = max;
    a.offset_min = off_min;
    a.offset_max = off_max;
    return a;
}

// The conversation on screen: the DialogueInput one, else the first active one.
const Dialogue* on_screen(World& w) {
    const Dialogue* found = nullptr;
    w.query<const Dialogue&, const DialogueInput&>().each([&](const Dialogue& d, const DialogueInput& in) {
        if (!found && in.enabled && d.active) found = &d;
    });
    if (!found)
        w.query<const Dialogue&>().each([&](const Dialogue& d) {
            if (!found && d.active) found = &d;
        });
    return found;
}

}  // namespace

DialogueBox::DialogueBox(const DialogueBoxStyle& style) : style_(style) {
    layer_.user = this;
    layer_.init = [](void* user, render::Renderer&, const ui::Skin& skin) {
        static_cast<DialogueBox*>(user)->init(skin);
        return true;
    };
    layer_.update = [](void* user, World& world, const ui::UpdateContext& ctx) { static_cast<DialogueBox*>(user)->update(world, ctx); };
    layer_.submit = [](void* user, render::SpriteBatch& batch, i16 sort_layer) { static_cast<DialogueBox*>(user)->submit(batch, sort_layer); };
}

void DialogueBox::init(const ui::Skin& skin) {
    doc_.set_skin(skin);
    doc_.set_modal(false);  // a view: it never takes focus or input
    const DialogueBoxStyle& st = style_;
    box_ = doc_.add(doc_.root(), ui::WidgetKind::Panel, "box");
    name_tag_ = doc_.add(box_, ui::WidgetKind::Panel, "name_tag");
    name_ = doc_.add(name_tag_, ui::WidgetKind::Text, "name");
    for (u32 i = 0; i < 4; ++i) {
        const char names[4][6] = {"line0", "line1", "line2", "line3"};
        lines_[i] = doc_.add(box_, ui::WidgetKind::Text, StringView(names[i]));
    }
    // The choices: four rows stacked above the box.
    choices_ = doc_.add(doc_.root(), ui::WidgetKind::Panel, "choices");
    doc_.set_style_all(choices_, flat({0, 0, 0, 0}, st.text, layout_.text_px, false));
    for (u32 i = 0; i < 4; ++i) {
        const char rows[4][5] = {"row0", "row1", "row2", "row3"};
        rows_[i] = doc_.add(choices_, ui::WidgetKind::Panel, StringView(rows[i]));
        row_text_[i] = doc_.add(rows_[i], ui::WidgetKind::Text, "text");
    }
    built_ = true;
    place(layout_);
    show(false);
}

// The geometry novel::dialogue_rects mirrors: change one, change the other.
void DialogueBox::place(const DialogueLayout& layout) {
    layout_ = layout;
    const DialogueBoxStyle& st = style_;
    const f32 px = layout.text_px, row_h = px + 18.0f;
    doc_.set_anchors(box_, band({0, 1}, {1, 1}, {24, -layout.box_height - 24}, {-24, -24}));
    doc_.set_style_all(box_, flat(st.box, st.text, px, true));
    doc_.set_anchors(name_tag_, band({0, 0}, {0, 0}, {16, -px - 10}, {16 + 12 * px, 6}));
    doc_.set_style_all(name_tag_, flat(st.name_tag, st.text, px, true));
    doc_.set_anchors(name_, band({0, 0}, {1, 1}, {12, 4}, {-12, -2}));
    doc_.set_style_all(name_, flat(st.text, st.text, px, false));
    for (u32 i = 0; i < 4; ++i) {
        const f32 y = 20.0f + static_cast<f32>(i) * (px + 12.0f);
        doc_.set_anchors(lines_[i], band({0, 0}, {1, 0}, {28, y}, {-28, y + px + 4}));
        doc_.set_style_all(lines_[i], flat(st.text, st.text, px, false));
    }
    const f32 list_h = 4 * row_h + 3 * 10.0f;
    doc_.set_anchors(choices_, band({0.5f, 1}, {0.5f, 1}, {-360, -layout.box_height - 48 - list_h}, {360, -layout.box_height - 48}));
    for (u32 i = 0; i < 4; ++i) {
        const f32 y = static_cast<f32>(i) * (row_h + 10.0f);
        doc_.set_anchors(rows_[i], band({0, 0}, {1, 0}, {0, y}, {0, y + row_h}));
        doc_.set_anchors(row_text_[i], band({0, 0}, {1, 1}, {18, 8}, {-18, -6}));
        doc_.set_style_all(row_text_[i], flat(st.text, st.text, px, false));
        row_look_[i] = 255;  // restyled on the next update
    }
    box_alpha_ = -1.0f;  // the box restyles on the next update
}

void DialogueBox::show(bool on) {
    visible_ = on;
    doc_.set_visible(box_, on);
    if (!on) {
        doc_.set_visible(choices_, false);
        options_ = 0;
        for (u16& l : page_) l = 0;
    }
}

void DialogueBox::update(World& world, const ui::UpdateContext& ctx) {
    if (!built_) return;
    const Program* p = dialogue_program(world);
    if (const DialogueLayout l = dialogue_layout(world); l.text_px != layout_.text_px || l.box_height != layout_.box_height) place(l);
    const Dialogue* d = p ? on_screen(world) : nullptr;
    show(d != nullptr);
    if (d) {
        const LocaleText* lt = story_locale(*p, world);  // the player's language (presentation only)
        const char* who = d->speaker == 255 ? "" : localized_speaker(*p, lt, d->speaker);
        // The player's subtitle settings (M-ACCESS): the speaker's name tag, and the box's
        // opacity scaled as access.subtitle_background scales the subtitle boxes (0.6 = as styled).
        const ui::SubtitleStyle sub = ui::subtitle_style();
        doc_.set_text(name_, StringView(who));
        doc_.set_visible(name_tag_, who[0] != '\0' && sub.speaker_names);
        const f32 alpha = math::clamp(style_.box.a * sub.background / ui::SubtitleStyle{}.background, 0.0f, 1.0f);
        if (alpha != box_alpha_) {
            box_alpha_ = alpha;
            math::Color box = style_.box;
            box.a = alpha;
            ui::Style look = flat(box, style_.text, layout_.text_px, true);
            look.text_outline = alpha < 0.5f ? 1.0f : 0.0f;  // no box to read against: outline the page
            doc_.set_style_all(box_, look);
            for (u32 i = 0; i < 4; ++i) {
                ui::Style t = flat(style_.text, style_.text, layout_.text_px, false);
                t.text_outline = look.text_outline;
                doc_.set_style_all(lines_[i], t);
            }
        }
        // The page typed out as far as the sim has revealed it. A choice with no prompt of its own
        // keeps the page before it up.
        const bool prompt = d->line[0] || d->line[1] || d->line[2] || d->line[3];
        if (prompt || !d->waiting_choice)
            for (u32 i = 0; i < 4; ++i) page_[i] = d->line[i];
        u16 page[4];
        localized_page(*p, lt, page_, page);  // in the player's language, re-read each update
        u32 shown_points = 0;
        for (u32 i = 0; i < 4; ++i) shown_points += page[i] ? utf8_points(StringView(text(*p, page[i]))) : 0;
        // The sim types the base page; a translation keeps pace with it.
        u32 left = d->waiting_choice ? ~0u : scale_reveal(dialogue_revealed(world, *d), dialogue_page_points(*p, *d), shown_points);
        for (u32 i = 0; i < 4; ++i) {
            const StringView full(page[i] ? text(*p, page[i]) : "");
            const u32 n = utf8_points(full);
            const u32 shown = left < n ? left : n;
            doc_.set_text(lines_[i], utf8_prefix(full, shown));
            left -= shown;
        }
        options_ = d->waiting_choice ? d->option_count : 0;
        doc_.set_visible(choices_, options_ > 0);
        const StoryState* vars = dialogue_vars(world);
        for (u32 i = 0; i < 4; ++i) {
            const bool on = i < options_;
            doc_.set_visible(rows_[i], on);
            if (!on) continue;
            const Option& o = p->options[d->options[i]];
            u16 t = 0, t2 = 0;
            localized_option(*p, lt, o, t, t2);
            doc_.set_text(row_text_[i], StringView(text(*p, t)));
            const bool open = !vars || test(*vars, *p, o.cond);
            const u8 look = !open ? 2 : i == d->cursor ? 1 : 0;
            if (row_look_[i] != look) {
                row_look_[i] = look;
                const math::Color tint = look == 1 ? style_.row_selected : style_.row;
                doc_.set_style_all(rows_[i], flat(tint, style_.text, layout_.text_px, true));
                doc_.set_style_all(row_text_[i], flat(tint, look == 2 ? style_.row_disabled_text : style_.text, layout_.text_px, false));
            }
        }
    }
    ui::UpdateContext view = ctx;
    view.actions = nullptr;  // the sim owns the input
    view.pointer = {-1, -1};
    view.pointer_down = view.pointer_pressed = view.pointer_released = false;
    doc_.update(view);
}

void DialogueBox::submit(render::SpriteBatch& batch, i16 sort_layer) const {
    if (built_ && visible_) doc_.submit(batch, sort_layer);
}

StringView DialogueBox::speaker() const { return visible_ ? doc_.text(name_) : StringView(); }
StringView DialogueBox::line(u32 i) const { return visible_ && i < 4 ? doc_.text(lines_[i]) : StringView(); }
StringView DialogueBox::option(u32 i) const { return i < options_ ? doc_.text(row_text_[i]) : StringView(); }

}  // namespace mce::novel
