// ui_screens sample game: gameplay talks to its UI documents only through data (M-UI-DATA step 2).
// It never includes mce/ui: it reads the input actions the documents' widgets raise ("Pause",
// "Resume", "Restart"), writes the Score the HUD binds, and shows the pause screen by setting
// its kit::UiScreen::visible. The stock mce_player builds, draws and feeds the documents.

#include "components.h"
#include "mce/ecs/transform.h"
#include "mce/gameplay.h"
#include "mce/input/actions.h"
#include "mce/kit/components.h"
#include "mce/log.h"

namespace ui_screens {

using namespace mce;

namespace {

// An action the tick's input carries (the gameplay map, or the UI map while a modal screen is up).
bool pressed(const input::InputState* in, const char* name) {
    const StringId id(name);
    return in && in->has_action(id) && in->pressed(id);
}

void play(Frame& f) {
    const input::InputState* in = f.try_resource<input::InputState>();
    bool open = false;
    f.query<kit::UiScreen&, PauseMenu&>().each([&](kit::UiScreen& screen, PauseMenu& menu) {
        if (screen.visible) {
            if (pressed(in, "Resume") || pressed(in, "Cancel") || pressed(in, "Restart")) screen.visible = false;
        } else if (pressed(in, "Pause")) {
            screen.visible = true;
            ++menu.opened;
        }
        open = screen.visible;
    });
    const bool restart = pressed(in, "Restart");
    f.query<Score&>().each([&](Score& s) {
        if (restart) s = Score{};
        if (open) return;
        s.time += f.dt;
        s.points = static_cast<i32>(s.time);
    });
    f.query<LocalTransform&>().with<kit::Sprite>().each([&](LocalTransform& t) {
        if (!open) t.rotation = math::quat_from_axis_angle({0, 0, 1}, f.dt) * t.rotation;
    });
}

}  // namespace

ResultV install(World& world) {
    world.register_components<Score, PauseMenu>();
    world.add_system(Phase::FixedUpdate_Pre, "ui_screens_play", &play);
    MCE_LOG_INFO("ui_screens: game systems installed");
    return ok();
}

}  // namespace ui_screens

MCE_GAME_MODULE("ui_screens", 1, &ui_screens::install)
