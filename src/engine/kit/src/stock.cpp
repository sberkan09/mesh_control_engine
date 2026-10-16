#include "mce/kit/stock.h"

#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/cinematic.h"
#include "mce/kit/components.h"
#include "mce/kit/captions.h"
#include "mce/kit/effects.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/joints3d.h"
#include "mce/kit/ragdolls3d.h"
#include "mce/kit/sensors3d.h"
#include "mce/kit/vehicles3d.h"
#include "mce/kit/nav3d.h"
#include "mce/kit/particles.h"
#include "mce/kit/presence.h"
#include "mce/kit/sequence.h"
#include "mce/kit/skeleton2d.h"
#include "mce/kit/timeline.h"
#include "mce/kit/ai.h"
#include "mce/kit/video.h"
#include "mce/kit/sprite_anim.h"
#include "mce/physics/world2d.h"
#include "mce/scene/scene.h"

namespace mce::kit {

static_assert(sizeof(WorldSettings) == 24);

namespace {

struct WorldSettingsInstalled {};

void kit_world_settings(Frame& f) {
    bool found = false;
    f.query<const WorldSettings&>().each([&](const WorldSettings& s) {
        if (found) return;
        found = true;
        if (physics::World2D* w2 = f.try_resource<physics::World2D>()) w2->set_gravity(s.gravity_2d);
        if (Physics3D* p3 = f.try_resource<Physics3D>()) p3->world.set_gravity(s.gravity_3d);
    });
}

struct UiFlowInstalled {};

bool pressed(const input::InputState* in, const FixedString<32>& action) {
    if (!in || action.empty()) return false;
    const StringId id(action.view());
    return in->has_action(id) && in->pressed(id);
}

// Before the tick's gameplay: each UiFlow screen reacts to the actions this tick carries (the state
// it started the tick in decides, so one action never shows and hides the same screen), then the
// world holds while a pause screen is visible.
void kit_ui_flow(Frame& f) {
    const input::InputState* in = f.try_resource<input::InputState>();
    bool hold = false;
    asset::Guid load;
    f.query<const UiFlow&, UiScreen&>().each([&](const UiFlow& flow, UiScreen& screen) {
        if (screen.visible && pressed(in, flow.hide_on)) screen.visible = false;
        else if (!screen.visible && pressed(in, flow.show_on)) screen.visible = true;
        if (!flow.scene.guid.is_null() && pressed(in, flow.load_on) && load.is_null()) load = flow.scene.guid;
        if (pressed(in, flow.quit_on)) scene::request_quit(f.world());
        hold = hold || (screen.visible && flow.pause);
    });
    if (!load.is_null()) {
        scene::request_change(f.world(), load);
        hold = false;  // the next scene starts from its own screens
    }
    f.world().set_held(hold);
}

}  // namespace

void install_ui_flow(World& world) {
    if (world.try_resource<UiFlowInstalled>()) return;
    world.set_resource(UiFlowInstalled{});
    world.register_components<UiScreen, UiFlow>();
    world.add_system(Phase::FixedUpdate_Pre, "kit_ui_flow", &kit_ui_flow);
    world.run_when_held(Phase::FixedUpdate_Pre, "kit_ui_flow");
}

void install_world_settings(World& world) {
    if (world.try_resource<WorldSettingsInstalled>()) return;
    world.set_resource(WorldSettingsInstalled{});
    world.register_components<WorldSettings>();
    world.add_system(Phase::Startup, "kit_world_settings", &kit_world_settings);
}

void install_stock(World& world, asset::AssetSystem* assets) {
    install_particles(world, assets);
    install_sequences(world, assets);
    install_video(world, assets);  // before the timelines: a video track's playhead wins the tick
    install_timelines(world, assets);  // before kit3d: an animation key starts its clip the same tick
    install_ai(world, assets);  // before the path and navmesh agents: a move the tree asks for starts the same tick
    install_kit3d(world, assets);
    install_bodies3d(world, assets);
    install_nav3d(world, assets);
    install_joints3d(world, assets);
    install_ragdolls3d(world, assets);
    install_vehicles3d(world, assets);
    install_sensors3d(world, assets);
    install_world_settings(world);
    install_ui_flow(world);
    install_effects(world, assets);
    install_sprite_animation(world, assets);
    install_skeleton2d(world, assets);  // after the timelines: a key on Skeleton2D.pose poses the same tick
    install_presence(world);
    install_captions(world);  // after kit_effects: this tick's SfxTrigger plays are captioned
    install_cinematic(world);  // last: kit_cinematic runs after the timelines and the follow cameras
}

}  // namespace mce::kit
