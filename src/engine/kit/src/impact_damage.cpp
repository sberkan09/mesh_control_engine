#include "mce/kit/impact_damage.h"

#include "mce/containers/array.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/seats3d.h"
#include "mce/kit/traffic.h"
#include "mce/kit/vehicles3d.h"

namespace mce::kit {

// Padding-free: World::state_hash hashes component bytes.
static_assert(sizeof(ImpactDamage3D) == 16);

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
    // The person inside feels it too: the driver, when the car has one (never the instigator's own seat).
    if (dmg->occupant_share <= 0.0f) return;
    const VehicleSeat3D* seat = w.try_get<VehicleSeat3D>(self);
    if (!seat || seat->driver.is_null() || !w.alive(seat->driver) || seat->driver == who) return;
    DamageInfo od = d;
    od.amount = d.amount * dmg->occupant_share;
    apply_damage(w, seat->driver, od);
}

void kit_impact_damage(Frame& f) {
    World& w = f.world();
    // A character (a pedestrian, the player) has no RigidBody3D flag to opt in with: ImpactDamage3D is the opt-in.
    if (Physics3D* phys = w.try_resource<Physics3D>())
        f.query<const ImpactDamage3D, const CharacterController3D>().each([&](Entity e, const ImpactDamage3D& d, const CharacterController3D&) {
            phys->world.set_contact_events(e, d.enabled ? d.min_speed : -1.0f);
        });
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

// A car whose Health3D is dead is a wreck: nobody drives it any more (an AI driver lets go, the player's input is off),
// it rolls with no throttle and the handbrake on. Health3D::destroy_on_death / linger then remove it, or it stays as scenery.
void kit_wreck(Frame& f) {
    World& w = f.world();
    f.query<const Health3D>().each([&](Entity e, const Health3D& hp) {
        if (!hp.dead) return;
        Vehicle3D* v = w.try_get_mut<Vehicle3D>(e);
        if (!v) return;
        v->use_input = 0;
        v->throttle = 0.0f;
        v->steer = 0.0f;
        v->brake = 0.0f;
        v->handbrake = 1.0f;
        if (TrafficDriver3D* t = w.try_get_mut<TrafficDriver3D>(e)) t->drive = 0;
        if (VehicleDriver3D* d = w.try_get_mut<VehicleDriver3D>(e)) d->drive = 0;
    });
}

}  // namespace

bool repair_vehicle(World& w, Entity car) {
    Health3D* hp = car.is_null() || !w.alive(car) ? nullptr : w.try_get_mut<Health3D>(car);
    if (!hp) return false;
    hp->current = hp->max;
    hp->dead = 0;
    hp->dead_time = 0.0f;
    hp->stagger_left = 0.0f;
    if (Vehicle3D* v = w.try_get_mut<Vehicle3D>(car)) {
        v->handbrake = 0.0f;
        const VehicleSeat3D* seat = w.try_get<VehicleSeat3D>(car);
        v->use_input = seat && !seat->driver.is_null() ? 1 : 0;
    }
    if (ParticleEmitter3D* em = w.try_get_mut<ParticleEmitter3D>(car)) em->emitting = 0;
    return true;
}

void install_impact_damage(World& world, asset::AssetSystem* assets) {
    install_combat3d(world, assets);
    install_bodies3d(world, assets);
    if (world.try_resource<ImpactDamageState>()) return;
    world.set_resource(ImpactDamageState{});
    world.register_components<ImpactDamage3D>();
    world.add_system(Phase::FixedUpdate_Pre, "kit_impact_damage", &kit_impact_damage);
    world.add_system(Phase::FixedUpdate_Pre, "kit_wreck", &kit_wreck);
}

}  // namespace mce::kit
