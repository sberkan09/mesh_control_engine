#include "mce/kit/impact_damage.h"

#include "mce/containers/array.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/seats3d.h"

namespace mce::kit {

// Padding-free: World::state_hash hashes component bytes.
static_assert(sizeof(ImpactDamage3D) == 12);

namespace {

struct ImpactDamageState {
    u8 installed = 1;
};

constexpr f32 kRehitSeconds = 1.0f;

// Who gets the blame for `other` hitting `self`: other's driver when it has one, else other itself.
Entity instigator_of(World& w, Entity other) {
    if (const VehicleSeat3D* seat = w.try_get<VehicleSeat3D>(other))
        if (!seat->driver.is_null() && w.alive(seat->driver)) return seat->driver;
    return other;
}

void hit(World& w, Entity self, Entity other, const ContactBegin3D& ev, f32 normal_sign) {
    const ImpactDamage3D* dmg = w.try_get<ImpactDamage3D>(self);
    const Health3D* hp = w.try_get<Health3D>(self);
    if (!dmg || !hp || !dmg->enabled || hp->dead || ev.approach_speed < dmg->min_speed) return;
    const Entity who = instigator_of(w, other);
    if (hp->hits > 0 && hp->last_attacker == who && hp->since_hit < kRehitSeconds) return;
    DamageInfo d;
    d.amount = (ev.approach_speed - dmg->min_speed + 1.0f) * dmg->damage_per_ms;
    d.instigator = who;
    d.point = ev.point;
    d.direction = ev.normal * normal_sign;  // from the other body towards this one
    d.impulse = ev.impulse;
    apply_damage(w, self, d);
}

void kit_impact_damage(Frame& f) {
    World& w = f.world();
    if (!w.event_queue<ContactBegin3D>()) return;
    // Copy first: apply_damage pushes events of its own.
    Array<ContactBegin3D> seen;
    for (const ContactBegin3D& ev : f.events().read<ContactBegin3D>()) seen.push_back(ev);
    for (const ContactBegin3D& ev : seen) {
        if (!w.alive(ev.a) || !w.alive(ev.b)) continue;
        hit(w, ev.a, ev.b, ev, -1.0f);  // normal runs from a to b
        hit(w, ev.b, ev.a, ev, 1.0f);
    }
}

}  // namespace

void install_impact_damage(World& world, asset::AssetSystem* assets) {
    install_combat3d(world, assets);
    install_bodies3d(world, assets);
    if (world.try_resource<ImpactDamageState>()) return;
    world.set_resource(ImpactDamageState{});
    world.register_components<ImpactDamage3D>();
    world.add_system(Phase::FixedUpdate_Pre, "kit_impact_damage", &kit_impact_damage);
}

}  // namespace mce::kit
