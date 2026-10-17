#include "mce/kit/stock.h"

#include <cmath>
#include <optional>

#include "mce/audio/components.h"

#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/cinematic.h"
#include "mce/kit/coop.h"
#include "mce/kit/components.h"
#include "mce/kit/captions.h"
#include "mce/kit/effects.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/joints3d.h"
#include "mce/kit/ragdolls3d.h"
#include "mce/kit/crowd.h"
#include "mce/kit/roads.h"
#include "mce/kit/traffic.h"
#include "mce/kit/sensors3d.h"
#include "mce/kit/vehicles3d.h"
#include "mce/kit/seats3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/missions.h"
#include "mce/kit/wanted3d.h"
#include "mce/kit/nav3d.h"
#include "mce/kit/particles.h"
#include "mce/kit/presence.h"
#include "mce/kit/proclevel.h"
#include "mce/kit/sequence.h"
#include "mce/kit/skeleton2d.h"
#include "mce/kit/timeline.h"
#include "mce/kit/ui_flow.h"
#include "mce/physics/world2d.h"
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
    asset::Guid load;
    f.query<const UiFlow&, UiScreen&>().each([&](const UiFlow& flow, UiScreen& screen) {
        if (screen.visible && pressed(in, flow.hide_on)) screen.visible = false;
        else if (!screen.visible && pressed(in, flow.show_on)) screen.visible = true;
        if (!flow.scene.guid.is_null() && pressed(in, flow.load_on) && load.is_null()) load = flow.scene.guid;
        if (pressed(in, flow.quit_on)) scene::request_quit(f.world());
    });
    apply_ui_flow_rules(f, in, load);  // after the actions: a rule sees the screens as UiFlow left them
    bool hold = false;
    bool keep = false;
    f.query<const UiFlow&, const UiScreen&>().each([&](const UiFlow& flow, const UiScreen& screen) {
        if (!screen.visible || !flow.pause) return;
        hold = true;
        keep = keep || flow.keep_audio;
    });
    if (!load.is_null()) {
        scene::request_change(f.world(), load);
        hold = keep = false;  // the next scene starts from its own screens
    }
    f.world().set_held(hold);
    if (audio::PauseMix* mix = f.world().try_resource<audio::PauseMix>()) mix->keep = keep;
}

}  // namespace

void install_ui_flow(World& world) {
    if (world.try_resource<UiFlowInstalled>()) return;
    world.set_resource(UiFlowInstalled{});
    world.set_resource(audio::PauseMix{});
    world.register_components<UiScreen, UiFlow, UiFlowRules>();
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
    install_coop(world, assets);  // before the 3D controllers: its Move and Jump drive them the same tick
    install_bodies3d(world, assets);
    install_nav3d(world, assets);
    install_joints3d(world, assets);
    install_ragdolls3d(world, assets);
    install_roads(world, assets);
    install_traffic(world, assets);
    install_crowd(world, assets);  // after the road clock: pedestrians read this tick's lights
    install_vehicles3d(world, assets);
    install_seats3d(world, assets);
    install_combat3d(world, assets);
    install_missions(world, assets);
    install_wanted3d(world);
    install_sensors3d(world, assets);
    install_world_settings(world);
    install_ui_flow(world);
    install_effects(world, assets);
    install_sprite_animation(world, assets);
    install_skeleton2d(world, assets);  // after the timelines: a key on Skeleton2D.pose poses the same tick
    install_presence(world);
    install_proclevel(world);
    install_captions(world);  // after kit_effects: this tick's SfxTrigger plays are captioned
    install_cinematic(world);  // last: kit_cinematic runs after the timelines and the follow cameras
    install_audio_occlusion(world);
}

namespace {

// Whether anything in `mask` lies on the segment from the listener to the source (neither counts).
bool audio_occluded(World& w, Entity source, Entity listener, math::vec3 from, math::vec3 to, u16 mask) {
    const math::vec3 d = to - from;
    const f32 dist = math::length(d);
    if (dist < 1e-4f) return false;
    const Entity skip[2] = {source, listener};
    if (const Physics3D* w3 = w.try_resource<Physics3D>()) {
        physics::QueryFilter3D q;
        q.layer_mask = mask;
        q.exclude = Span<const Entity>(skip, 2);
        return w3->world.raycast(from, d * (1.0f / dist), dist, q).has_value();
    }
    if (const physics::World2D* w2 = w.try_resource<physics::World2D>()) {
        physics::QueryFilter q;
        q.layer_mask = mask;
        q.exclude = Span<const Entity>(skip, 2);
        // The 2D world is the XY plane: z is dropped, as everywhere else in 2D, so the ray is as
        // long as the plane distance (a 3D scene with only a World2D is read the same way).
        const f32 plane = std::sqrt(d.x * d.x + d.y * d.y);
        if (plane < 1e-4f) return false;
        return w2->raycast({from.x, from.y}, {d.x / plane, d.y / plane}, plane, q).has_value();
    }
    return false;
}

}  // namespace

void install_audio_occlusion(World& world) {
    if (audio::AudioRef* ref = world.try_resource<audio::AudioRef>()) ref->occlusion = &audio_occluded;
}

}  // namespace mce::kit
