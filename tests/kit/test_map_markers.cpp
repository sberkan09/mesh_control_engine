// Map markers (M-MISSION 2): a marker is its entity's position or follows an objective's target while that
// objective is active, a mission's marker walks its chain and goes away when it ends, and a scene without
// markers is the same as before.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/map_markers.h"
#include "mce/kit/missions.h"
#include "mce/scene/entity_names.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

// A player at the origin, a zone at (10, 0, -5), an enemy at (20, 0, 4); the chain reach -> kill.
struct Scene {
    World w;
    u64 t = 0;
    Entity player, zone, enemy, reach, kill, mission;
    explicit Scene(bool markers = true) {
        w.register_components<LocalTransform, SaveKey>();
        if (markers) install_map_markers(w, nullptr);
        else install_missions(w, nullptr);
        player = w.spawn_with(SaveKey{1}, at(0, 0, 0), Health3D{});
        zone = w.spawn_with(SaveKey{2}, at(10, 0, -5));
        Health3D weak;
        weak.max = weak.current = 10.0f;
        enemy = w.spawn_with(SaveKey{3}, at(20, 2, 4), weak);
        Objective o2;
        o2.kind = ObjectiveKind::Kill;
        o2.target = enemy;
        kill = w.spawn_with(SaveKey{4}, o2);
        Objective o1;
        o1.kind = ObjectiveKind::ReachZone;
        o1.target = zone;
        o1.radius = 2.0f;
        o1.next = kill;
        reach = w.spawn_with(SaveKey{5}, o1);
        Mission m;
        m.id = "intro";
        m.objective = reach;
        m.actor = player;
        m.start_on_load = 1;
        mission = w.spawn_with(SaveKey{6}, m);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    Array<MapMarkerView> markers() {
        Array<MapMarkerView> v;
        collect_map_markers(w, v);
        return v;
    }
    Mission& mis() { return *w.try_get_mut<Mission>(mission); }
};

}  // namespace

TEST_CASE("map markers: an unattached marker sits at its entity") {
    Scene s;
    MapMarker m;
    m.icon = 3;
    m.layer = 2;
    m.color = math::Color{1, 0, 0, 1};
    m.label = "shop";
    m.always_visible = 1;
    const Entity e = s.w.spawn_with(at(3, 7, -4), m);
    const Array<MapMarkerView> v = s.markers();
    REQUIRE(v.size() == 1);
    CHECK(v[0].source == e);
    CHECK(v[0].pos.x == 3.0f);
    CHECK(v[0].pos.y == -4.0f);  // z; the height is dropped
    CHECK(v[0].icon == 3);
    CHECK(v[0].layer == 2);
    CHECK(v[0].always_visible);
    CHECK(v[0].color.r == 1.0f);
    CHECK(v[0].label.view() == "shop");
}

TEST_CASE("map markers: a marker on a mission follows the active objective and ends with the mission") {
    Scene s;
    s.w.add(s.mission, MapMarker{});
    CHECK(s.markers().size() == 0);  // idle: nothing to show
    s.run(2);                        // started; the first objective is active
    CHECK(s.mis().state == MissionState::Active);
    {
        const Array<MapMarkerView> v = s.markers();
        REQUIRE(v.size() == 1);
        CHECK(v[0].source == s.mission);
        CHECK(v[0].pos.x == 10.0f);
        CHECK(v[0].pos.y == -5.0f);
    }
    s.w.try_get_mut<LocalTransform>(s.player)->position = math::vec3(9, 0, -5);  // within 2 m of the zone
    s.run(2);
    CHECK(s.mis().current == s.kill);
    {
        const Array<MapMarkerView> v = s.markers();
        REQUIRE(v.size() == 1);
        CHECK(v[0].pos.x == 20.0f);  // the enemy
        CHECK(v[0].pos.y == 4.0f);
    }
    s.w.try_get_mut<LocalTransform>(s.enemy)->position = math::vec3(21, 0, 6);  // it moves: the marker with it
    CHECK(s.markers()[0].pos.x == 21.0f);
    DamageInfo hit;
    hit.amount = 100.0f;
    hit.instigator = s.player;
    REQUIRE(apply_damage(s.w, s.enemy, hit));
    s.run(3);
    CHECK(s.mis().state == MissionState::Complete);
    CHECK(s.markers().size() == 0);
}

TEST_CASE("map markers: follow_objective on an objective shows its target only while it is active") {
    Scene s;
    MapMarker m;
    m.follow_objective = s.reach;
    s.w.spawn_with(m);  // a marker of its own, on no objective
    MapMarker bare;
    s.w.add(s.kill, bare);  // a bare marker on the second objective
    CHECK(s.markers().size() == 0);  // the mission is idle, both objectives pending
    s.run(2);
    {
        const Array<MapMarkerView> v = s.markers();
        REQUIRE(v.size() == 1);  // reach is active, kill is pending
        CHECK(v[0].pos.x == 10.0f);
        CHECK(v[0].pos.y == -5.0f);
    }
    s.w.try_get_mut<LocalTransform>(s.player)->position = math::vec3(10, 0, -5);
    s.run(2);
    {
        const Array<MapMarkerView> v = s.markers();  // reach is done: its marker is gone, kill's appears
        REQUIRE(v.size() == 1);
        CHECK(v[0].source == s.kill);
        CHECK(v[0].pos.x == 20.0f);
    }
}

TEST_CASE("map markers: an objective found by tag is followed once the entity exists") {
    Scene s;
    Objective o;
    o.kind = ObjectiveKind::Signal;
    o.tag = "crate";
    const Entity obj = s.w.spawn_with(o);
    s.w.try_get_mut<Objective>(obj)->state = ObjectiveState::Active;
    MapMarker m;
    m.follow_objective = obj;
    s.w.spawn_with(m);
    CHECK(s.markers().size() == 0);  // no entity named crate yet
    const Entity crate = s.w.spawn_with(at(-6, 1, 8));
    scene::EntityNames names;
    names.rows.push_back(scene::EntityNames::Row{StringId("crate").value, crate});
    s.w.set_resource(names);
    const Array<MapMarkerView> v = s.markers();
    REQUIRE(v.size() == 1);
    CHECK(v[0].pos.x == -6.0f);
    CHECK(v[0].pos.y == 8.0f);
}

TEST_CASE("map markers: a scene without markers is unchanged") {
    Scene with(true), without(false);
    with.run(30);
    without.run(30);
    CHECK(with.markers().size() == 0);
    CHECK(with.mis().ticks == without.mis().ticks);
    CHECK(with.mis().state == without.mis().state);
    CHECK(with.w.state_hash() == without.w.state_hash());
}

TEST_CASE("map markers: the tracked view reads position and yaw") {
    Scene s;
    scene::EntityNames names;
    names.rows.push_back(scene::EntityNames::Row{StringId("Player").value, s.player});
    s.w.set_resource(names);
    math::vec2 pos;
    f32 heading = 99.0f;
    REQUIRE(map_tracked_view(s.w, "Player", pos, heading));
    CHECK(pos.x == 0.0f);
    CHECK(heading == doctest::Approx(0.0f).epsilon(1e-6));  // faces -z: north
    LocalTransform* t = s.w.try_get_mut<LocalTransform>(s.player);
    t->position = math::vec3(4, 0, -2);
    t->rotation = math::quat_from_axis_angle(math::vec3(0, 1, 0), 1.5707964f);  // a quarter turn left: faces -x
    REQUIRE(map_tracked_view(s.w, "Player", pos, heading));
    CHECK(pos.x == 4.0f);
    CHECK(pos.y == -2.0f);
    CHECK(heading == doctest::Approx(1.5707964f).epsilon(1e-5));
    CHECK_FALSE(map_tracked_view(s.w, "nobody", pos, heading));
}
