#pragma once
// The stock player's kit::UiScreen support (M-UI-DATA step 2): every UiScreen of the scene is a
// .mcui document built through mce_ui, drawn over the game and fed the pointer and the UI action
// map. Gameplay sees only data: the screen's `visible` flag, read-only "bind" paths, and the
// actions the widgets raise, folded into the next tick's InputState (ui::ActionRelay).
//
// Headless runs decode the documents without building them: no widget is ever drawn or clicked
// there, but the actions they name are declared, so the tick snapshots (and the state hash) are
// the same as a windowed run's.

#include "mce/asset/asset_system.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/render_textures.h"
#include "mce/kit/sprite_extract.h"
#include "mce/kit/views.h"
#include "mce/ui/options_panel.h"
#include "mce/ui/screen_link.h"
#include "mce/ui/ui.h"

namespace mce::player {

class UiScreens {
public:
    UiScreens() = default;
    UiScreens(const UiScreens&) = delete;
    UiScreens& operator=(const UiScreens&) = delete;
    ~UiScreens() { clear(); }

    // The scene's screens: `entities` are scene::load's. With `skin` (a windowed run) the documents
    // are built; `extract` resolves a document's font.
    Result<void> load(World& world, asset::AssetSystem& assets, Span<const Entity> entities, const ui::Skin* skin,
                      kit::SpriteExtract* extract);
    // -ui_preview: the one document `guid`, always visible, over no world (the MCP server's
    // ui_preview captures it). Its bindings still read the scene's entities.
    Result<void> load_preview(World& world, asset::AssetSystem& assets, Span<const Entity> entities, asset::Guid guid,
                              const ui::Skin* skin, kit::SpriteExtract* extract);
    void clear();
    bool active() const { return !screens_.empty(); }
    // The input system a document's remap list drives (M-ACCESS: ui::OptionsPanel); set before load.
    void set_input(input::InputSystem* input) { input_ = input; }

    // Before the tick's systems: the raised actions into the snapshot.
    void apply_actions(input::InputState& state, f32 dt) { relay_.apply(state, dt); }
    // Per rendered frame, before update: where each screen with a UiScreenView lays out — its
    // camera's window view in `views` (none: the whole window). `display_scale` and `user_scale`
    // pick a view's UI scale as compute_ui_scale does for the window.
    void set_views(World& world, Span<const kit::ViewEntry> views, f32 display_scale, f32 user_scale);
    // Per rendered frame: visibility from the components, pending bindings, update, events -> actions.
    // A screen in a view lays out in its rect: the pointer is moved into it and reaches it only inside.
    void update(World& world, const ui::UpdateContext& ctx);
    // A visible modal screen: the host switches input to the "UI" map while one is up.
    bool modal_visible() const;
    // The document focus lives in: the topmost visible modal screen's (null when none is up). The
    // screen reader speaks its focus (M-ACCESS).
    const ui::Document* focus_document() const;
    // Per rendered frame, before submit: image widgets named like a RenderTexture's `ui_image` show
    // that texture (M-VIEWS 3); a name whose texture is gone falls back to the skin's image.
    void bind_render_textures(const kit::RenderTextures& rts);
    // Each full-window screen on its own layers below `base`, by order (two layers + two per open modal).
    void submit(render::SpriteBatch& batch, i16 base) const;
    // The visible screens laid out in a view: the host draws each in a pass whose viewport is its
    // rect, into a batch begun with compute_screen_view(rect size), under the window's screens.
    u32 view_screen_count() const;
    rhi::Rect2D view_screen_rect(u32 i) const;
    void submit_view_screen(u32 i, render::SpriteBatch& batch, i16 layer) const;

private:
    struct Screen;
    // `theme`: the scene's theme for the document (UiScreen::theme); null: the document's own.
    Result<void> add(Entity e, asset::AssetSystem& assets, asset::Guid guid, asset::Guid theme, i32 order, bool visible,
                     const ui::Skin* skin, kit::SpriteExtract* extract, Screen*& out);
    struct Screen {
        Entity entity;
        i32 order = 0;
        bool visible = true;
        bool modal = false;
        bool bindings_pending = true;
        bool always_visible = false;  // a preview: no UiScreen component behind it
        bool in_view = false;         // laid out in `rect` (UiScreenView), not the whole window
        rhi::Rect2D rect;
        f32 view_ui_scale = 1.0f;
        ui::Document* doc = nullptr;  // null headless
        ui::OptionsPanel* options = nullptr;  // a document with a remap list (the stock options screen)
    };
    const Screen* view_screen(u32 i) const;  // the i-th visible screen laid out in a view
    Array<Screen> screens_;
    Array<Entity> entities_;
    Array<FixedString<32>> rt_images_;  // the ui_image names bound last frame
    ui::ActionRelay relay_;
    input::InputSystem* input_ = nullptr;
};

}  // namespace mce::player
