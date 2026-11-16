// Floater (M-FLOAT 55b): floater_pose agrees with the water queries, kit_floater writes only LocalTransform y and
// rotation, keeps the heading, snaps with follow 1 and smooths below it, skips disabled / child / water-less cases,
// and the component round-trips defaults.

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/floater.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/water.h"
#include "mce/kit/water_flow.h"
#include "mce/kit/weather.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

struct Rig {
    World world;
    u64 t = 0;
    Rig() {
        world.register_components<SaveKey>();
        install_kit3d(world, nullptr);
        install_weather(world, nullptr);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, t++);
    }
    Entity water(bool wind_follow = false) {
        Water w;
        w.wind_follow = wind_follow ? 1.0f : 0.0f;
        w.wind_dir_deg = 30.0f;
        w.amplitude = 1.0f;
        w.level_y = 2.0f;
        return world.spawn_with(SaveKey{1}, w);
    }
    Entity floater(math::vec3 p, Floater f = {}) {
        LocalTransform lt;
        lt.position = p;
        return world.spawn_with(lt, f);
    }
    LocalTransform& lt(Entity e) { return *world.try_get_mut<LocalTransform>(e); }
};

f32 len(math::vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

}  // namespace

TEST_CASE("floater: defaults") {
    const Floater f;
    CHECK(f.offset_y == 0.0f);
    CHECK(f.tilt == 1.0f);
    CHECK(f.follow == 1.0f);
    CHECK(f.enabled == 1);
    CHECK(f.pad_[0] == 0);
}

TEST_CASE("floater_pose: height and up come from the water queries") {
    Water w;
    w.amplitude = 1.0f;
    w.level_y = 2.0f;
    Floater fl;
    fl.offset_y = 0.5f;
    const math::vec3 p{3.0f, 9.0f, -4.0f};
    const f64 t = 7.25;
    const FloaterPose pose = floater_pose(w, nullptr, fl, p, t, 30.0f);
    CHECK(pose.y == water_height_at(w, nullptr, {3.0f, -4.0f}, t, 30.0f) + 0.5f);
    const math::vec3 n = water_normal_at(w, nullptr, {3.0f, -4.0f}, t, 30.0f);
    CHECK(pose.up.x == doctest::Approx(n.x).epsilon(1e-5));
    CHECK(pose.up.y == doctest::Approx(n.y).epsilon(1e-5));
    CHECK(pose.up.z == doctest::Approx(n.z).epsilon(1e-5));
    CHECK(len(pose.up) == doctest::Approx(1.0f));
}

TEST_CASE("floater_pose: tilt 0 stays upright, NaN authoring falls back") {
    Water w;
    w.amplitude = 1.0f;
    Floater fl;
    fl.tilt = 0.0f;
    const FloaterPose up = floater_pose(w, nullptr, fl, {1.0f, 0.0f, 1.0f}, 3.0, 0.0f);
    CHECK(up.up.x == 0.0f);
    CHECK(up.up.y == 1.0f);
    CHECK(up.up.z == 0.0f);
    fl.tilt = std::numeric_limits<f32>::quiet_NaN();
    fl.offset_y = std::numeric_limits<f32>::quiet_NaN();
    const FloaterPose nan = floater_pose(w, nullptr, fl, {1.0f, 0.0f, 1.0f}, 3.0, 0.0f);
    const FloaterPose def = floater_pose(w, nullptr, Floater{}, {1.0f, 0.0f, 1.0f}, 3.0, 0.0f);
    CHECK(nan.y == def.y);
    CHECK(nan.up.x == def.up.x);
}

TEST_CASE("floater_pose: flat water gives level + offset and straight up") {
    Water w;
    w.amplitude = 0.0f;
    w.level_y = 5.0f;
    Floater fl;
    fl.offset_y = -1.0f;
    const FloaterPose pose = floater_pose(w, nullptr, fl, {8.0f, 0.0f, 8.0f}, 12.0, 0.0f);
    CHECK(pose.y == 4.0f);
    CHECK(pose.up.y == 1.0f);
}

TEST_CASE("floater_rotation: keeps the heading and leans toward up") {
    const math::quat yaw = math::quat_from_axis_angle({0, 1, 0}, 1.0f);
    const math::quat r0 = floater_rotation(yaw, {0, 1, 0});
    CHECK(math::near(r0, yaw, 1e-5f));
    const math::vec3 up = math::normalize(math::vec3{0.3f, 1.0f, -0.2f});
    const math::quat r = floater_rotation(yaw, up);
    const math::vec3 axis = math::rotate(r, {0, 1, 0});
    CHECK(axis.x == doctest::Approx(up.x).epsilon(1e-4));
    CHECK(axis.y == doctest::Approx(up.y).epsilon(1e-4));
    CHECK(axis.z == doctest::Approx(up.z).epsilon(1e-4));
    // Same heading: the twist about Y of the result is the yaw (a second pass is a fixed point).
    CHECK(math::near(floater_rotation(r, up), r, 1e-4f));
}

TEST_CASE("kit_floater: rides the surface, writes only y and rotation") {
    Rig rig;
    rig.water();
    const Entity e = rig.floater({3.0f, 0.0f, -4.0f});
    rig.lt(e).scale = {2.0f, 2.0f, 2.0f};
    rig.run(90);
    Water w;
    w.wind_follow = 0.0f;
    w.wind_dir_deg = 30.0f;
    w.amplitude = 1.0f;
    w.level_y = 2.0f;
    // The system ran on tick 89 (scene time 89 * dt): the last write is that surface height.
    const f32 want = water_height_at(w, nullptr, {3.0f, -4.0f}, 89.0 * static_cast<f64>(kDt), 30.0f);
    CHECK(rig.lt(e).position.y == doctest::Approx(want).epsilon(1e-5));
    CHECK(rig.lt(e).position.x == 3.0f);
    CHECK(rig.lt(e).position.z == -4.0f);
    CHECK(rig.lt(e).scale.x == 2.0f);
    CHECK(len({rig.lt(e).rotation.x, rig.lt(e).rotation.y, rig.lt(e).rotation.z}) > 0.0f);
}

TEST_CASE("kit_floater: disabled, child, and water-less entities are left alone") {
    Rig rig;
    Floater off;
    off.enabled = 0;
    const Entity a = rig.floater({0.0f, 7.0f, 0.0f}, off);
    const Entity parent = rig.floater({0.0f, 0.0f, 0.0f}, off);
    const Entity b = rig.floater({0.0f, 7.0f, 0.0f});
    rig.world.add(b, ChildOf{parent});
    rig.run(5);  // no Water yet
    CHECK(rig.lt(a).position.y == 7.0f);
    CHECK(rig.lt(b).position.y == 7.0f);
    rig.water();
    rig.run(5);
    CHECK(rig.lt(a).position.y == 7.0f);  // enabled 0
    CHECK(rig.lt(b).position.y == 7.0f);  // child
    CHECK(rig.lt(a).rotation == math::quat{});
}

TEST_CASE("kit_floater: follow below 1 smooths toward the surface") {
    Rig rig;
    rig.water();
    Floater slow;
    slow.follow = 0.1f;
    const Entity fast = rig.floater({0.0f, 20.0f, 0.0f});
    const Entity soft = rig.floater({0.0f, 20.0f, 0.0f}, slow);
    rig.run(1);
    CHECK(rig.lt(fast).position.y < 5.0f);
    CHECK(rig.lt(soft).position.y > 15.0f);
    CHECK(rig.lt(soft).position.y < 20.0f);
    rig.run(200);
    CHECK(std::abs(rig.lt(soft).position.y - rig.lt(fast).position.y) < 1.5f);
}

TEST_CASE("kit_floater: two identical runs match bit for bit") {
    auto run = [] {
        Rig rig;
        rig.water();
        const Entity e = rig.floater({1.0f, 0.0f, 2.0f});
        rig.run(120);
        return rig.lt(e);
    };
    const LocalTransform a = run();
    const LocalTransform b = run();
    CHECK(a.position == b.position);
    CHECK(a.rotation == b.rotation);
}
