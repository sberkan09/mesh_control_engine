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
        // Sprites: the document's sheet, else the theme's (apply_theme copied it into `data`). Style
        // sprite regions index it; a sheet that does not load, or whose white region is out of
        // range, leaves the built-in skin (flat quads).
        if (!data.sprites.is_null()) {
            u32 regions = 0;
            const render::SpriteSetId set = extract ? extract->ui_sheet_set(data.sprites, &regions) : render::SpriteSetId{};
            if (set.valid() && data.white_region < regions) {
                look.set = set;
                look.white_region = data.white_region;
            } else {
                MCE_LOG_WARN("player: UI document {}: its sprite sheet {} does not load{}; using the built-in skin",
                             asset::to_string(guid).view(), asset::to_string(data.sprites).view(),
                             set.valid() ? " (white_region is past its regions)" : "");
            }
        }
        sc.doc = new ui::Document();
        sc.doc->set_skin(look);
        if (Result<void> r = ui::build_document(data, *sc.doc); !r) {
            delete sc.doc;
            return r;
        }
        sc.minimap.bind(*sc.doc);
        sc.items.bind(*sc.doc);
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

void UiScreens::set_views(World& world, Span<const kit::ViewEntry> views, f32 display_scale, f32 user_scale,
                          f32 reference_height) {
    for (Screen& s : screens_) {
        s.in_view = false;
        const kit::UiScreenView* sv = s.always_visible ? nullptr : world.try_get<kit::UiScreenView>(s.entity);
        if (!sv) continue;
        for (const kit::ViewEntry& v : views) {
            if (v.camera != sv->camera || !v.target.is_null()) continue;
            s.in_view = true;
            s.rect = v.rect;
            s.view_ui_scale = ui::compute_ui_scale({v.rect.width, v.rect.height}, display_scale, user_scale, {360, 640},
                                                    reference_height);
            break;
        }
    }
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
        if (s.in_view) {
            // The view's rect: its own size and scale, the safe area only on the window's edges.
            const i32 w = static_cast<i32>(ctx.viewport.width), h = static_cast<i32>(ctx.viewport.height);
            const rhi::Rect2D& r = s.rect;
            c.viewport = {r.width, r.height};
            c.ui_scale = s.view_ui_scale;
            c.safe_insets.left = r.x <= 0 ? ctx.safe_insets.left : 0.0f;
            c.safe_insets.top = r.y <= 0 ? ctx.safe_insets.top : 0.0f;
            c.safe_insets.right = r.x + static_cast<i32>(r.width) >= w ? ctx.safe_insets.right : 0.0f;
            c.safe_insets.bottom = r.y + static_cast<i32>(r.height) >= h ? ctx.safe_insets.bottom : 0.0f;
            const math::vec2 p{ctx.pointer.x - static_cast<f32>(r.x), ctx.pointer.y - static_cast<f32>(r.y)};
            const bool inside = p.x >= 0.0f && p.y >= 0.0f && p.x < static_cast<f32>(r.width) && p.y < static_cast<f32>(r.height);
            c.pointer = inside ? p : math::vec2{-1.0f, -1.0f};
            if (!inside) c.pointer_pressed = false;
        }
        if (!s.visible || (top_modal && &s != top_modal)) {
            c.actions = nullptr;
            c.pointer = {-1, -1};
            c.pointer_down = c.pointer_pressed = c.pointer_released = false;
            c.wheel = 0.0f;
        }
        if (s.visible && !s.items.empty()) s.items.feed(world, *s.doc, s.entity);  // M-INVENTORY 4b
        if (s.visible && !s.minimap.empty()) s.minimap.feed(world, *s.doc);  // M-MISSION 3b: before the document draws
        s.doc->update(c);
        if (s.options && s.visible) s.options->update();
        if (s.visible) relay_.collect(*s.doc);
    }
}

void UiScreens::bind_render_textures(const kit::RenderTextures& rts) {
    for (const FixedString<32>& name : rt_images_)
        for (Screen& s : screens_)
            if (s.doc) s.doc->set_image_texture(name.view(), {});
    rt_images_.clear();
    for (const kit::RenderTextures::Entry& e : rts.entries()) {
        if (e.ui_image.empty() || !e.set.valid()) continue;
        rt_images_.push_back(e.ui_image);
        for (Screen& s : screens_)
            if (s.doc) s.doc->set_image_texture(e.ui_image.view(), rts.sprite_set(e.entity));
    }
}

u32 UiScreens::view_screen_count() const {
    u32 n = 0;
    for (const Screen& s : screens_) n += s.doc && s.visible && s.in_view ? 1u : 0u;
    return n;
}

const UiScreens::Screen* UiScreens::view_screen(u32 i) const {
    for (const Screen& s : screens_)
        if (s.doc && s.visible && s.in_view && i-- == 0) return &s;
    return nullptr;
}

rhi::Rect2D UiScreens::view_screen_rect(u32 i) const {
    const Screen* s = view_screen(i);
    return s ? s->rect : rhi::Rect2D{};
}

void UiScreens::submit_view_screen(u32 i, render::SpriteBatch& batch, i16 layer) const {
    if (const Screen* s = view_screen(i)) s->doc->submit(batch, layer);
}

void UiScreens::submit(render::SpriteBatch& batch, i16 base) const {
    i32 layer = base;
    for (const Screen& s : screens_) {
        if (!s.doc || !s.visible || s.in_view) continue;
        s.doc->submit(batch, static_cast<i16>(layer));
        layer += 6;  // quads + text, and one open modal's two layers above them, plus a spare pair
    }
}

}  // namespace mce::player
