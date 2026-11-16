#include "mce/kit/fall_death.h"

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"

namespace mce::kit {

static_assert(sizeof(FallDeath3D) == 8);

namespace {

struct FallDeathInstalled {};

void kit_fall_death(Frame& f) {
    World& w = f.world();
    f.query<FallDeath3D, LocalTransform>().each([&](Entity e, FallDeath3D& fd, LocalTransform& t) {
        if (t.position.y >= fd.min_y) return;
        const Health3D* h = w.try_get<Health3D>(e);
        if (!h || h->dead) return;
        DamageInfo d;
        d.amount = 1.0e9f;
        d.point = t.position;
        d.direction = {0.0f, -1.0f, 0.0f};
        apply_damage(w, e, d);
    });
}

}  // namespace

void install_fall_death(World& world) {
    if (world.try_resource<FallDeathInstalled>()) return;
    world.set_resource(FallDeathInstalled{});
    world.register_components<FallDeath3D>();
    world.add_system(Phase::FixedUpdate_Pre, "kit_fall_death", &kit_fall_death, "kit3d_health");
}

}  // namespace mce::kit
