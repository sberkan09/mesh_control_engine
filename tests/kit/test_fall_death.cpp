// Fall death (M-FALLDEATH): dropping below min_y is lethal through apply_damage; a Respawn3D wakes the player.

#include <doctest/doctest.h>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/fall_death.h"
#include "mce/kit/kit3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

struct Scene {
    World w;
    u64 t = 0;
    Scene() {
        w.register_components<LocalTransform>();
        install_combat3d(w, nullptr);
        install_fall_death(w);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    Entity body(f32 y, u8 invulnerable = 0) {
        Health3D h;
        h.invulnerable = invulnerable;
        return w.spawn_with(at(0, y, 0), h, FallDeath3D{});
    }
    bool dead(Entity e) { return w.try_get<Health3D>(e)->dead == 1; }
};

}  // namespace

TEST_CASE("fall_death: above min_y nothing happens, below it the entity dies") {
    Scene s;
    const Entity safe = s.body(-29.0f);
    const Entity gone = s.body(-31.0f);
    s.run(3);
    CHECK_FALSE(s.dead(safe));
    CHECK(s.dead(gone));
    CHECK(s.w.try_get<Health3D>(gone)->current == 0.0f);
}

TEST_CASE("fall_death: an invulnerable entity keeps falling and the event fires once") {
    Scene s;
    const Entity ghost = s.body(-100.0f, 1);
    const Entity gone = s.body(-100.0f);
    s.run(5);
    CHECK_FALSE(s.dead(ghost));
    CHECK(s.dead(gone));
    CHECK(s.w.try_get<Health3D>(gone)->hits == 1);
}
