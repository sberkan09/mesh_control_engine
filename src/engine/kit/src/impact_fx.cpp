#include "mce/kit/impact_fx.h"

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/effects.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/particles.h"

namespace mce::kit {

static_assert(sizeof(ImpactFx3D) == 16);

namespace {

struct ImpactFxInstalled {};

void kit_impact_fx(Frame& f) {
    World& w = f.world();
    auto ev = f.events();
    bool any = false;
    math::vec3 at, last_normal;
    for (const WeaponHit3D& h : ev.read<WeaponHit3D>()) {  // the last hit of the tick wins
        any = true;
        at = h.point;
        last_normal = h.normal;
    }
    ParticleSystem* particles = w.try_resource<ParticleSystem>();
    f.query<ImpactFx3D>().each([&](Entity e, ImpactFx3D& fx) {
        if (fx.pending) {  // the previous tick moved the emitter; its world transform is current now
            if (particles) particles->burst(e, fx.pending);
            if (ParticleEmitter3D* em = w.try_get_mut<ParticleEmitter3D>(e)) {
                em->burst = fx.pending;
                ++em->burst_version;
            }
            fx.pending = 0;
            ++fx.fired;
        }
        if (!any) return;
        if (LocalTransform* t = w.try_get_mut<LocalTransform>(e)) t->position = at + last_normal * fx.lift;
        fx.pending = fx.count;
    });
}

}  // namespace

void install_impact_fx(World& world) {
    if (world.try_resource<ImpactFxInstalled>()) return;
    world.set_resource(ImpactFxInstalled{});
    world.register_components<ImpactFx3D>();
    world.add_system(Phase::FixedUpdate_Post, "kit_impact_fx", &kit_impact_fx, "kit_effects");
}

}  // namespace mce::kit
