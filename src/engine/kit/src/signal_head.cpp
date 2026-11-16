#include "mce/kit/signal_head.h"

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"

namespace mce::kit {

bool signal_approach_light(const RoadNet& net, math::vec3 position, math::vec3 approach, RoadLight& out, f32 reach) {
    out = RoadLight::Green;
    if (!net.def) return false;
    if (!(approach.x * approach.x + approach.z * approach.z > 1e-8f)) return false;
    RoadNearestQuery q;
    q.max_distance = reach;
    q.heading = {approach.x, 0.0f, approach.z};
    RoadSpot spot;
    if (!road_nearest(net, position, spot, q)) return false;
    const RoadRegistry::Def& d = *net.def;
    const asset::RoadLane& lane = d.lanes[spot.lane];
    if (lane.kind != asset::RoadLaneKind::Road) return false;
    bool any_green = false, any_amber = false, any = false;
    for (u32 k = 0; k < lane.next_count; ++k) {
        const u32 next = lane.first_next + k;
        if (next >= d.lanes.size()) break;
        any = true;
        const RoadLight l = road_light(net, next);
        any_green = any_green || l == RoadLight::Green;
        any_amber = any_amber || l == RoadLight::Amber;
    }
    out = !any || any_green ? RoadLight::Green : any_amber ? RoadLight::Amber : RoadLight::Red;
    return true;
}

SignalLampLook signal_lamp_look(RoadLight lamp, RoadLight state) {
    if (lamp != state) return {{0.08f, 0.08f, 0.08f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    switch (lamp) {
        case RoadLight::Green: return {{0.20f, 1.00f, 0.35f, 1.0f}, {0.0f, 6.0f, 0.0f}};
        case RoadLight::Amber: return {{1.00f, 0.70f, 0.10f, 1.0f}, {6.0f, 4.0f, 0.0f}};
        case RoadLight::Red: break;
    }
    return {{1.00f, 0.12f, 0.10f, 1.0f}, {6.0f, 0.0f, 0.0f}};
}

namespace {

void kit_signal_heads(Frame& f) {
    World& w = f.world();
    RoadNet first;
    const bool have_first = road_net_first(w, first);
    f.query<LocalTransform, MeshRef, const SignalHead>().without<ChildOf>().each(
        [&](Entity, LocalTransform& lt, MeshRef& mesh, const SignalHead& head) {
            if (head.enabled == 0) return;
            RoadNet net;
            const RoadNet* use = nullptr;
            if (head.network.is_null()) {
                if (have_first) use = &first;
            } else if (road_net(w, head.network, net)) {
                use = &net;
            }
            if (!use) return;
            RoadLight state;
            if (!signal_approach_light(*use, lt.position, head.approach, state)) return;
            const SignalLampLook look = signal_lamp_look(head.lamp, state);
            mesh.tint = look.tint;
            mesh.emissive_tint = look.emissive_tint;
        });
}

}  // namespace

void install_signal_head(World& world) {
    world.register_components<SignalHead, MeshRef>();
    world.add_system(Phase::FixedUpdate_Pre, "kit_signal_heads", &kit_signal_heads, "roads_clock");
}

}  // namespace mce::kit
