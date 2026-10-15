// kit::UiFlow (M-UI-DATA demo): screens shown, hidden and held on input actions with no game code,
// and the world's held ticks (World::set_held) that a pause screen sets.

#include <doctest/doctest.h>

#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/input/ecs.h"
#include "mce/kit/components.h"
#include "mce/kit/stock.h"
#include "mce/scene/scene.h"

using namespace mce;

namespace {

u32 g_gameplay_runs = 0;
void gameplay(Frame&) { ++g_gameplay_runs; }

// One tick whose input presses `action` (none: an empty snapshot).
void tick(World& w, u64& t, const char* action = nullptr) {
    input::InputState in;
    in.tick = t;
    if (action) {
        in.action_count = 1;
        in.action_names[0] = StringId(action);
        in.actions[0].held = in.actions[0].pressed = true;
    }
    input::publish(w, in);
    w.tick(1.0 / 60.0, t++);
}

kit::UiScreen& screen(World& w, Entity e) { return *w.try_get_mut<kit::UiScreen>(e); }

}  // namespace

TEST_CASE("ui flow: a title holds the game until Play, Pause holds it again until Resume, a scene loads on Restart") {
    World w;
    input::install(w);
    kit::install_ui_flow(w);
    w.add_system(Phase::FixedUpdate_Pre, "gameplay", &gameplay);
    g_gameplay_runs = 0;

    kit::UiFlow title_flow;
    title_flow.hide_on = "Play";
    title_flow.pause = true;
    const Entity title = w.spawn();
    w.add(title, kit::UiScreen{});
    w.add(title, title_flow);

    kit::UiFlow pause_flow;
    pause_flow.show_on = "Pause";
    pause_flow.hide_on = "Resume";
    pause_flow.load_on = "Restart";
    pause_flow.scene.guid = asset::Guid{1, 2};
    pause_flow.pause = true;
    kit::UiScreen hidden;
    hidden.visible = false;
    const Entity pause = w.spawn();
    w.add(pause, hidden);
    w.add(pause, pause_flow);

    u64 t = 0;
    for (u32 i = 0; i < 5; ++i) tick(w, t);
    CHECK(w.held());  // the title is up: gameplay never ran
    CHECK(g_gameplay_runs == 0);
    tick(w, t, "Play");
    CHECK_FALSE(screen(w, title).visible);
    CHECK_FALSE(w.held());
    CHECK(g_gameplay_runs == 1);  // the rest of the Play tick runs
    for (u32 i = 0; i < 4; ++i) tick(w, t);
    CHECK(g_gameplay_runs == 5);

    tick(w, t, "Pause");  // shown and held from this tick on: gameplay runs after the flow no more
    CHECK(screen(w, pause).visible);
    CHECK(w.held());
    for (u32 i = 0; i < 3; ++i) tick(w, t);
    CHECK(g_gameplay_runs == 5);
    CHECK(w.current_tick() == t - 1);  // held ticks still count

    tick(w, t, "Resume");
    CHECK_FALSE(screen(w, pause).visible);
    CHECK(g_gameplay_runs == 6);

    tick(w, t, "Pause");
    tick(w, t, "Restart");  // loads the scene, and the hold lets go
    const scene::SceneChange* change = w.try_resource<scene::SceneChange>();
    REQUIRE(change);
    CHECK(change->next.view() == "guid:00000000-0000-0001-0000-000000000002");
    CHECK_FALSE(w.held());
}

TEST_CASE("ui flow: one action never shows and hides a screen in the same tick; a toggle needs two presses") {
    World w;
    input::install(w);
    kit::install_ui_flow(w);
    kit::UiFlow toggle;
    toggle.show_on = toggle.hide_on = "Map";
    kit::UiScreen hidden;
    hidden.visible = false;
    const Entity map = w.spawn();
    w.add(map, hidden);
    w.add(map, toggle);
    u64 t = 0;
    tick(w, t, "Map");
    CHECK(screen(w, map).visible);
    CHECK_FALSE(w.held());  // not a pause screen
    tick(w, t);
    CHECK(screen(w, map).visible);
    tick(w, t, "Map");
    CHECK_FALSE(screen(w, map).visible);
}

TEST_CASE("ui flow: quit_on asks the host to end the run, once (SBA-61)") {
    World w;
    input::install(w);
    kit::install_ui_flow(w);
    kit::UiFlow title;
    title.hide_on = "Play";
    title.quit_on = "Quit";
    const Entity e = w.spawn();
    w.add(e, kit::UiScreen{});
    w.add(e, title);
    u64 t = 0;
    tick(w, t);
    CHECK_FALSE(scene::take_quit_request(w));
    tick(w, t, "Quit");
    CHECK(scene::take_quit_request(w));
    CHECK_FALSE(scene::take_quit_request(w));  // taken: the host acts on it once
}
