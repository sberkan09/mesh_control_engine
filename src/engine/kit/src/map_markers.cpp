#include "mce/kit/map_markers.h"

#include <cmath>

#include "kit3d_internal.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/missions.h"
#include "mce/scene/entity_names.h"

namespace mce::kit {
namespace {

struct MapMarkersInstalled {};

bool position_of(World& w, Entity e, math::vec3& pos, math::quat* rot = nullptr) {
    if (e.is_null() || !w.alive(e)) return false;
    const LocalTransform* t = w.try_get<LocalTransform>(e);
    if (!t) return false;
    math::quat q;
    math::vec3 scale;
    world_pose(w, e, *t, pos, q, scale);
    if (rot) *rot = q;
    return true;
}

// The entity an active objective points at, or null.
Entity objective_target(World& w, Entity objective) {
    const Objective* o = objective.is_null() || !w.alive(objective) ? nullptr : w.try_get<Objective>(objective);
    if (!o || o->state != ObjectiveState::Active) return Entity{};
    if (!o->target.is_null()) return o->target;
    return o->tag.empty() ? Entity{} : scene::find_entity(w, o->tag.view());
}

// The entity a followed Objective or Mission shows, or null when it shows nothing.
Entity followed_target(World& w, Entity follow) {
    if (follow.is_null() || !w.alive(follow)) return Entity{};
    if (w.try_get<Objective>(follow)) return objective_target(w, follow);
    if (const Mission* m = w.try_get<Mission>(follow))
        return m->state == MissionState::Active ? objective_target(w, m->current) : Entity{};
    return Entity{};
}

}  // namespace

bool map_marker_view(World& world, Entity marker, MapMarkerView& out) {
    if (marker.is_null() || !world.alive(marker)) return false;
    const MapMarker* m = world.try_get<MapMarker>(marker);
    if (!m) return false;
    Entity follow = m->follow_objective;
    if (follow.is_null() && (world.try_get<Objective>(marker) || world.try_get<Mission>(marker))) follow = marker;
    math::vec3 p;
    if (follow.is_null()) {
        if (!position_of(world, marker, p)) return false;
    } else {
        if (!position_of(world, followed_target(world, follow), p)) return false;
    }
    out.source = marker;
    out.pos = math::vec2(p.x, p.z);
    out.color = m->color;
    out.icon = m->icon;
    out.layer = m->layer;
    out.always_visible = m->always_visible != 0;
    out.label = m->label;
    return true;
}

void collect_map_markers(World& world, Array<MapMarkerView>& out) {
    out.clear();
    Array<Entity> markers;
    world.query<const MapMarker>().each([&](Entity e, const MapMarker&) { markers.push_back(e); });
    for (const Entity e : markers) {
        MapMarkerView v;
        if (map_marker_view(world, e, v)) out.push_back(v);
    }
}

bool map_tracked_view(World& world, StringView name, math::vec2& pos, f32& heading) {
    const Entity e = scene::find_entity(world, name);
    math::vec3 p;
    math::quat q;
    if (!position_of(world, e, p, &q)) return false;
    const math::vec3 fwd = math::rotate(q, math::vec3(0, 0, -1));
    pos = math::vec2(p.x, p.z);
    heading = std::atan2(-fwd.x, -fwd.z);
    return true;
}

void install_map_markers(World& world, asset::AssetSystem* assets) {
    install_missions(world, assets);
    if (world.try_resource<MapMarkersInstalled>()) return;
    world.set_resource(MapMarkersInstalled{});
    world.register_components<MapMarker>();
}

}  // namespace mce::kit
