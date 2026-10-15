// kit::UiScreen in the stock player (M-UI-DATA step 2; ui_screens.h).

#include "ui_screens.h"

#include "mce/kit/components.h"
#include "mce/log.h"
#include "mce/ui/document_data.h"

namespace mce::player {

Result<void> UiScreens::add(Entity e, asset::AssetSystem& assets, asset::Guid guid, asset::Guid scene_theme, i32 order,
                            bool visible, const ui::Skin* skin, kit::SpriteExtract* extract, Screen*& out) {
    MCE_TRY_ASSIGN(const asset::AssetHandle<asset::UiDocument> h, assets.load_sync<asset::UiDocument>(guid));
    ui::DocumentData data;
    const Result<void> decoded = ui::decode_ui_document(assets.resolve(h), data);
    assets.release(asset::AnyHandle(h));
    MCE_TRY(decoded);
    for (const ui::WidgetData& w : data.widgets) relay_.declare(w.action.view());
    // The theme: the scene's (UiScreen::theme), else the document's. A theme that does not load
    // leaves the document's own look.
    const asset::Guid theme_guid = !scene_theme.is_null() ? scene_theme : data.theme;
    ui::ThemeData theme = ui::default_theme();
    if (!theme_guid.is_null()) {
        bool themed = false;
        const Result<asset::AssetHandle<asset::UiTheme>> th = assets.load_sync<asset::UiTheme>(theme_guid);
        if (th) {
            const Result<void> tr = ui::decode_ui_theme(assets.resolve(th.value()), theme);
            assets.release(asset::AnyHandle(th.value()));
            if (tr) {
                ui::apply_theme(theme, data);
                themed = true;
            }
        }
        if (!themed) {
            theme = ui::default_theme();
            MCE_LOG_WARN("player: UI document {}: its theme does not load; using the document's own look",
                         asset::to_string(guid).view());
        }
    }
    Screen sc;
    sc.entity = e;
    sc.order = order;
    sc.visible = visible;
    sc.modal = data.modal;
    if (skin) {
        ui::Skin look = *skin;
        // Fonts: the document's names the body font; the theme's fill the roles the document
        // leaves to it (body when the document has none, title, button).
        auto load_font = [&](asset::Guid g) -> const render::Font* {
            if (g.is_null()) return nullptr;
            const render::Font* f = extract ? extract->font(g) : nullptr;
            if (!f)
                MCE_LOG_WARN("player: UI document {}: a font does not load; using the built-in one",
                             asset::to_string(guid).view());
            return f;
        };
        const asset::Guid body = !data.font.is_null() ? data.font : theme.font(ui::FontRole::Body);
        if (const render::Font* f = load_font(body)) look.font = f;
        look.title_font = load_font(theme.font(ui::FontRole::Title));
        look.button_font = load_font(theme.font(ui::FontRole::Button));
        if (!data.sprites.is_null())
            MCE_LOG_WARN("player: UI document {}: sprite-sheet skins are not drawn yet; using the built-in one",
                         asset::to_string(guid).view());
        sc.doc = new ui::Document();
        sc.doc->set_skin(look);
        if (Result<void> r = ui::build_document(data, *sc.doc); !r) {
            delete sc.doc;
            return r;
        }
        if (input_) {
            sc.options = new ui::OptionsPanel();
            if (!sc.options->attach(*sc.doc, *input_)) {
                delete sc.options;
                sc.options = nullptr;
            }
        }
    }
    screens_.push_back(sc);
    out = &screens_.back();
    return {};
}

Result<void> UiScreens::load(World& world, asset::AssetSystem& assets, Span<const Entity> entities, const ui::Skin* skin,
                             kit::SpriteExtract* extract) {
    clear();
    for (const Entity e : entities) entities_.push_back(e);
    for (const Entity e : entities) {
        const kit::UiScreen* s = world.try_get<kit::UiScreen>(e);
        if (!s) continue;
        Screen* added = nullptr;
        MCE_TRY(add(e, assets, s->document.guid, s->theme.guid, s->order, s->visible, skin, extract, added));
    }
    // Draw (and hit-test) order: by `order`, then scene order. Insertion sort keeps ties stable.
    for (usize i = 1; i < screens_.size(); ++i)
        for (usize j = i; j > 0 && screens_[j - 1].order > screens_[j].order; --j) {
            const Screen t = screens_[j - 1];
            screens_[j - 1] = screens_[j];
            screens_[j] = t;
        }
    if (!screens_.empty())
        MCE_LOG_INFO("player: {} UI screen(s), {} action(s)", screens_.size(), relay_.declared_count());
    return {};
}

Result<void> UiScreens::load_preview(World&, asset::AssetSystem& assets, Span<const Entity> entities, asset::Guid guid,
                                     const ui::Skin* skin, kit::SpriteExtract* extract) {
    clear();
    for (const Entity e : entities) entities_.push_back(e);
    Screen* added = nullptr;
    MCE_TRY(add(Entity{}, assets, guid, {}, 0, true, skin, extract, added));
    added->always_visible = true;
    MCE_LOG_INFO("player: previewing UI document {}", asset::to_string(guid).view());
    return {};
}

void UiScreens::clear() {
    for (Screen& s : screens_) {
        delete s.options;
        delete s.doc;
    }
    screens_.clear();
    entities_.clear();
    relay_.clear();
}

bool UiScreens::modal_visible() const {
    for (const Screen& s : screens_)
        if (s.visible && s.modal) return true;
    return false;
}

const ui::Document* UiScreens::focus_document() const {
    const ui::Document* top = nullptr;
    for (const Screen& s : screens_)
        if (s.visible && s.modal && s.doc) top = s.doc;
    return top;
}

void UiScreens::update(World& world, const ui::UpdateContext& ctx) {
    // Pointer and focus go to the topmost visible modal only; a HUD below it still updates its bindings.
    const Screen* top_modal = nullptr;
    for (Screen& s : screens_) {
        if (!s.always_visible) {
            const kit::UiScreen* c = world.try_get<kit::UiScreen>(s.entity);
            s.visible = c && c->visible;
        }
        if (s.visible && s.modal) top_modal = &s;
    }
    for (Screen& s : screens_) {
        if (!s.doc) continue;
        if (s.bindings_pending) s.bindings_pending = ui::bind_document(*s.doc, world, s.entity, entities_.span()) != 0;
        ui::UpdateContext c = ctx;
        if (!s.visible || (top_modal && &s != top_modal)) {
            c.actions = nullptr;
            c.pointer = {-1, -1};
            c.pointer_down = c.pointer_pressed = c.pointer_released = false;
            c.wheel = 0.0f;
        }
        s.doc->update(c);
        if (s.options && s.visible) s.options->update();
        if (s.visible) relay_.collect(*s.doc);
    }
}

void UiScreens::submit(render::SpriteBatch& batch, i16 base) const {
    i32 layer = base;
    for (const Screen& s : screens_) {
        if (!s.doc || !s.visible) continue;
        s.doc->submit(batch, static_cast<i16>(layer));
        layer += 6;  // quads + text, and one open modal's two layers above them, plus a spare pair
    }
}

}  // namespace mce::player
