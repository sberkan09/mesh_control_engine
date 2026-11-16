#include "mce/kit/tracer3d.h"

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/kit3d.h"

namespace mce::kit {

static_assert(sizeof(Tracer3D) == 20);

namespace {

struct TracersInstalled {};

// The rotation that turns +Z onto `d` (unit), the shortest way.
math::quat z_to(math::vec3 d) {
    if (d.z > 0.99999f) return math::quat::identity();
    if (d.z < -0.99999f) return math::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 3.14159265f);
    const math::vec3 axis = math::normalize(math::vec3{-d.y, d.x, 0.0f});  // cross((0,0,1), d)
    return math::quat_from_axis_angle(axis, std::acos(d.z));
}

void kit_tracers(Frame& f) {
    World& w = f.world();
    auto ev = f.events();
    f.query<Tracer3D, MeshRef>().each([&](Entity, Tracer3D& t, MeshRef& m) {
        if (t.age < 0.0f) return;
        t.age += f.dt;
        if (t.age >= t.life) {
            t.age = -1.0f;
            m.visible = 0;
        }
    });
    for (const WeaponFired3D& shot : ev.read<WeaponFired3D>()) {
        const math::vec3 span = shot.end - shot.origin;
        const f32 len = math::length(span);
        if (!(len > 0.0f)) continue;
        u32 top = 0;
        Entity pick;
        u32 pick_serial = 0xFFFFFFFFu;
        f.query<Tracer3D, MeshRef, LocalTransform>().each([&](Entity e, Tracer3D& t, MeshRef&, LocalTransform&) {
            if (t.serial > top) top = t.serial;
            if (t.serial < pick_serial) {
                pick_serial = t.serial;
                pick = e;
            }
        });
        if (pick.is_null()) return;
        Tracer3D& t = *w.try_get_mut<Tracer3D>(pick);
        if (len < t.min_length) continue;
        MeshRef& m = *w.try_get_mut<MeshRef>(pick);
        LocalTransform& tr = *w.try_get_mut<LocalTransform>(pick);
        tr.position = shot.origin + span * 0.5f;
        tr.rotation = z_to(span / len);
        tr.scale = {t.width, t.width, len};
        m.visible = 1;
        t.age = 0.0f;
        t.serial = top + 1;
    }
}

}  // namespace

void install_tracers(World& world) {
    if (world.try_resource<TracersInstalled>()) return;
    world.set_resource(TracersInstalled{});
    world.register_components<Tracer3D>();
    world.add_system(Phase::FixedUpdate_Post, "kit_tracers", &kit_tracers);
}

}  // namespace mce::kit
