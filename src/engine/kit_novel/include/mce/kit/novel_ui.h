#pragma once
// The default NPC dialogue box (M-NOVEL, #154): a view on mce_ui of the conversation on screen, for
// games that run dialogue (mce/kit/novel.h install_dialogue) outside a novel stage. A host adds it
// to the stock player; the game module never includes it (mce_ui is the host's, FR-UI-030):
//
//   // the host's main.cpp, with mce_add_project(... HOST_DEPS mce::mce_kit_novel_ui)
//   mce::novel::DialogueBox box;
//   const mce::ui::Layer* layers[] = {box.layer()};
//   desc.ui_layers = layers;
//   desc.ui_layer_count = 1;
//
// It shows the Dialogue whose entity carries DialogueInput (else the first active one): the
// speaker's name, the page typed out as the sim reveals it (dialogue_revealed), and while a
// choice waits the options with the highlighted one marked and gated ones greyed. It reads the
// world and never writes it; Enter and up/down reach the conversation through DialogueInput in
// the sim, so a replay or a headless run plays the same.

#include "mce/ecs/world.h"
#include "mce/ui/ui.h"

namespace mce::novel {

struct DialogueBoxStyle {
    math::Color box{0.06f, 0.055f, 0.07f, 0.88f};
    math::Color name_tag{0.32f, 0.22f, 0.14f, 1.0f};
    math::Color text{0.96f, 0.93f, 0.86f, 1.0f};
    math::Color row{0.16f, 0.14f, 0.15f, 0.92f};
    math::Color row_selected{0.62f, 0.44f, 0.22f, 1.0f};
    math::Color row_disabled_text{0.55f, 0.52f, 0.5f, 1.0f};
    f32 text_px = 26.0f;
    f32 box_height = 190.0f;  // pixels at UI scale 1
};

class DialogueBox {
public:
    explicit DialogueBox(const DialogueBoxStyle& style = {});
    DialogueBox(const DialogueBox&) = delete;
    DialogueBox& operator=(const DialogueBox&) = delete;

    // Builds the widgets with a skin (the player's built-in one through layer()).
    void init(const ui::Skin& skin);
    // Reads the world's conversation into the widgets, then lays them out.
    void update(World& world, const ui::UpdateContext& ctx);
    void submit(render::SpriteBatch& batch, i16 sort_layer = 100) const;

    // What the box shows after update() (tests read it; "" when hidden).
    bool visible() const { return visible_; }
    StringView speaker() const;
    StringView line(u32 i) const;
    StringView option(u32 i) const;
    u32 option_count() const { return options_; }
    ui::Document& document() { return doc_; }

    // For player::PlayerDesc::ui_layers.
    const ui::Layer* layer() { return &layer_; }

private:
    void show(bool on);
    DialogueBoxStyle style_;
    ui::Document doc_;
    ui::Layer layer_;
    ui::WidgetId box_, name_tag_, name_, lines_[4], choices_, rows_[4], row_text_[4];
    u8 row_look_[4] = {255, 255, 255, 255};  // 0 normal, 1 selected, 2 disabled
    bool built_ = false, visible_ = false;
    u32 options_ = 0;
    u16 page_[4] = {};  // the base lines on the box (kept up under a choice with no prompt)
};

}  // namespace mce::novel
