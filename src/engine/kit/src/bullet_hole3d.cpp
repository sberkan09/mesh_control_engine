#include "mce/kit/bullet_hole3d.h"

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/kit3d.h"

namespace mce::kit {

static_assert(sizeof(BulletHole3D) == 28);

namespace {

struct BulletHolesInstalled {};

// The rotation that turns +Y onto `n` (unit), the shortest way.
math::quat up_to(math::vec3 n) {
    const f32 d = n.y;
    if (d > 0.99999f) return math::quat::identity();
    if (d < -0.99999f) return math::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, 3.14159265f);
    const math::vec3 axis = math::normalize(math::vec3{n.z, 0.0f, -n.x});  // cross((0,1,0), n)
    return math::quat_from_axis_angle(axis, std::acos(d));
}

void kit_bullet_holes(Frame& f) {
    World& w = f.world();
    auto ev = f.events();
    // Age first, so a hole placed this tick starts at 0.
    f.query<BulletHole3D, Decal3D>().each([&](Entity, BulletHole3D& h, Decal3D& d) {
        if (h.age < 0.0f) return;
        h.age += f.dt;
        const f32 over = h.age - h.life;
        if (over >= h.fade) {
            h.age = -1.0f;
            d.enabled = 0;
            return;
        }
        const f32 k = over <= 0.0f || h.fade <= 0.0f ? 1.0f : 1.0f - over / h.fade;
        d.color.w = h.alpha * k;
    });
    for (const WeaponHit3D& hit : ev.read<WeaponHit3D>()) {
        if (!hit.target.is_null() && w.alive(hit.target) && w.try_get<Health3D>(hit.target)) continue;
        u32 top = 0;
        Entity pick;
        u32 pick_serial = 0xFFFFFFFFu;
        f.query<BulletHole3D, Decal3D, LocalTransform>().each(
            [&](Entity e, BulletHole3D& h, Decal3D&, LocalTransform&) {
                if (h.serial > top) top = h.serial;
                if (h.serial < pick_serial) {
                    pick_serial = h.serial;
                    pick = e;
                }
            });
        if (pick.is_null()) return;
        BulletHole3D& h = *w.try_get_mut<BulletHole3D>(pick);
        Decal3D& d = *w.try_get_mut<Decal3D>(pick);
        LocalTransform& t = *w.try_get_mut<LocalTransform>(pick);
        const math::vec3 n = math::normalize(hit.normal);
        t.position = hit.point;
        t.rotation = up_to(n);
        d.size = {h.size, h.depth, h.size};
        d.color.w = h.alpha;
        d.enabled = 1;
        h.age = 0.0f;
        h.serial = top + 1;
    }
}

}  // namespace

void install_bullet_holes(World& world) {
    if (world.try_resource<BulletHolesInstalled>()) return;
    world.set_resource(BulletHolesInstalled{});
    world.register_components<BulletHole3D>();
    world.add_system(Phase::FixedUpdate_Post, "kit_bullet_holes", &kit_bullet_holes);
}

}  // namespace mce::kit
