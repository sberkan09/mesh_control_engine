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
#include "mce/kit/sprite_extract.h"
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
    void clear();
    bool active() const { return !screens_.empty(); }

    // Before the tick's systems: the raised actions into the snapshot.
    void apply_actions(input::InputState& state, f32 dt) { relay_.apply(state, dt); }
    // Per rendered frame: visibility from the components, pending bindings, update, events -> actions.
    void update(World& world, const ui::UpdateContext& ctx);
    // A visible modal screen: the host switches input to the "UI" map while one is up.
    bool modal_visible() const;
    // Each screen on its own layers below `base`, by order (two layers + two per open modal).
    void submit(render::SpriteBatch& batch, i16 base) const;

private:
    struct Screen {
        Entity entity;
        i32 order = 0;
        bool visible = true;
        bool modal = false;
        bool bindings_pending = true;
        ui::Document* doc = nullptr;  // null headless
    };
    Array<Screen> screens_;
    Array<Entity> entities_;
    ui::ActionRelay relay_;
};

}  // namespace mce::player
