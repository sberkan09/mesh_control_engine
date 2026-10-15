// kit::UiScreen in the stock player (M-UI-DATA step 2; ui_screens.h).

#include "ui_screens.h"

#include "mce/kit/components.h"
#include "mce/log.h"
#include "mce/ui/document_data.h"

namespace mce::player {

Result<void> UiScreens::load(World& world, asset::AssetSystem& assets, Span<const Entity> entities, const ui::Skin* skin,
                             kit::SpriteExtract* extract) {
    clear();
    for (const Entity e : entities) entities_.push_back(e);
    for (const Entity e : entities) {
        const kit::UiScreen* s = world.try_get<kit::UiScreen>(e);
        if (!s) continue;
        const asset::Guid guid = s->document.guid;
        MCE_TRY_ASSIGN(const asset::AssetHandle<asset::UiDocument> h, assets.load_sync<asset::UiDocument>(guid));
        ui::DocumentData data;
        const Result<void> decoded = ui::decode_ui_document(assets.resolve(h), data);
        assets.release(asset::AnyHandle(h));
        MCE_TRY(decoded);
        for (const ui::WidgetData& w : data.widgets) relay_.declare(w.action.view());
        Screen sc;
        sc.entity = e;
        sc.order = s->order;
        sc.visible = s->visible;
        sc.modal = data.modal;
        if (skin) {
            ui::Skin look = *skin;
            if (!data.font.is_null()) {
                const render::Font* f = extract ? extract->font(data.font) : nullptr;
                if (f) look.font = f;
                else MCE_LOG_WARN("player: UI document {}: its font does not load; using the built-in one",
                                  asset::to_string(guid).view());
            }
            if (!data.sprites.is_null())
                MCE_LOG_WARN("player: UI document {}: sprite-sheet skins are not drawn yet; using the built-in one",
                             asset::to_string(guid).view());
            sc.doc = new ui::Document();
            sc.doc->set_skin(look);
            if (Result<void> r = ui::build_document(data, *sc.doc); !r) {
                delete sc.doc;
                return r;
            }
        }
        screens_.push_back(sc);
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

void UiScreens::clear() {
    for (Screen& s : screens_) delete s.doc;
    screens_.clear();
    entities_.clear();
    relay_.clear();
}

bool UiScreens::modal_visible() const {
    for (const Screen& s : screens_)
        if (s.visible && s.modal) return true;
    return false;
}

void UiScreens::update(World& world, const ui::UpdateContext& ctx) {
    // Pointer and focus go to the topmost visible modal only; a HUD below it still updates its bindings.
    const Screen* top_modal = nullptr;
    for (Screen& s : screens_) {
        const kit::UiScreen* c = world.try_get<kit::UiScreen>(s.entity);
        s.visible = c && c->visible;
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
