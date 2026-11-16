#include "mce/kit/floater.h"

#include <cmath>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/wind_direction.h"

namespace mce::kit {

FloaterPose floater_pose(const Water& water, const WaterFlow* flow, const Floater& floater, math::vec3 position,
                         f64 scene_time, f32 wind_dir_deg) {
    const f32 offset = detail::water_clamp(floater.offset_y, 0.0f, -50.0f, 50.0f);
    const f32 tilt = detail::water_clamp(floater.tilt, 1.0f, 0.0f, 1.0f);
    const math::vec2 xz{position.x, position.z};
    FloaterPose out;
    out.y = water_height_at(water, flow, xz, scene_time, wind_dir_deg) + offset;
    const math::vec3 n = water_normal_at(water, flow, xz, scene_time, wind_dir_deg);
    math::vec3 u{n.x * tilt, 1.0f + (n.y - 1.0f) * tilt, n.z * tilt};
    const f32 len = std::sqrt(u.x * u.x + u.y * u.y + u.z * u.z);
    if (len > 0.0f && u.y > 0.0f) {
        out.up = {u.x / len, u.y / len, u.z / len};
    }
    return out;
}

math::quat floater_rotation(math::quat current, math::vec3 up) {
    // Twist about world Y: the part of `current` that turns about the vertical axis.
    const f32 tl = std::sqrt(current.y * current.y + current.w * current.w);
    const math::quat twist = tl > 1e-6f ? math::quat{0.0f, current.y / tl, 0.0f, current.w / tl} : math::quat{};
    // Shortest rotation from (0, 1, 0) to `up`: axis (up.z, 0, -up.x), w = 1 + up.y (up.y > 0, so never degenerate).
    const math::quat lean = math::normalize(math::quat{up.z, 0.0f, -up.x, 1.0f + up.y});
    return math::normalize(lean * twist);
}

namespace {

void kit_floater(Frame& f) {
    const Water* water = nullptr;
    f.query<const Water>().each([&](Entity, const Water& w) {
        if (!water) water = &w;
    });
    if (!water || water->enabled == 0) return;
    const WaterFlow* flow = nullptr;
    f.query<const WaterFlow>().each([&](Entity, const WaterFlow& w) {
        if (!flow) flow = &w;
    });
    f32 wind_deg = water->wind_dir_deg;
    if (water->wind_follow > 0.0f) {
        bool found = false;
        f.query<const WindDirection>().each([&](Entity, const WindDirection& c) {
            if (found) return;
            found = true;
            wind_deg = static_cast<f32>(static_cast<f64>(c.angle_q) / 65536.0);
        });
    }
    const f64 t = static_cast<f64>(f.tick) * static_cast<f64>(f.dt);
    f.query<LocalTransform, const Floater>().without<ChildOf>().each([&](Entity, LocalTransform& lt, const Floater& fl) {
        if (fl.enabled == 0) return;
        const FloaterPose pose = floater_pose(*water, flow, fl, lt.position, t, wind_deg);
        const f32 follow = detail::water_clamp(fl.follow, 1.0f, 0.0f, 1.0f);
        const math::quat target = floater_rotation(lt.rotation, pose.up);
        if (follow >= 1.0f) {
            lt.position.y = pose.y;
            lt.rotation = target;
        } else {
            lt.position.y += (pose.y - lt.position.y) * follow;
            lt.rotation = math::slerp(lt.rotation, target, follow);
        }
    });
}

}  // namespace

void install_floater(World& world) {
    world.register_components<Floater>();
    world.add_system(Phase::FixedUpdate_Pre, "kit_floater", &kit_floater, "kit_wind_direction");
}

}  // namespace mce::kit
