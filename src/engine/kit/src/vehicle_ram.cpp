#include "mce/kit/vehicle_ram.h"

#include <cmath>

#include "mce/containers/array.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/seats3d.h"
#include "mce/kit/vehicles3d.h"

namespace mce::kit {

// Padding-free: World::state_hash hashes component bytes.
static_assert(sizeof(VehicleRam3D) == 28);

namespace {

struct VehicleRamState {
    u8 installed = 1;
};

constexpr f32 kRehitSeconds = 1.0f;  // one car hurts the same victim once per second at most

struct Blow {
    Entity target;
    Entity car;
    Entity instigator;
    f32 amount = 0.0f;
    f32 impulse = 0.0f;
    math::vec3 point;
    math::vec3 direction;
};

void kit_vehicle_ram(Frame& f) {
    World& w = f.world();
    Array<Blow> blows;
    f.query<const Vehicle3D, const VehicleRam3D, const LocalTransform>().each(
        [&](Entity car, const Vehicle3D& v, const VehicleRam3D& ram, const LocalTransform& lt) {
            const f32 speed = std::fabs(v.speed);
            if (!ram.enabled || !std::isfinite(v.speed) || speed < ram.min_speed) return;
            const f32 dir = v.speed >= 0.0f ? 1.0f : -1.0f;
            const math::vec3 fwd = math::rotate(lt.rotation, math::vec3{0.0f, 0.0f, -dir});
            const math::vec3 right = math::rotate(lt.rotation, math::vec3{1.0f, 0.0f, 0.0f});
            Entity instigator = car;
            if (const VehicleSeat3D* seat = w.try_get<VehicleSeat3D>(car))
                if (!seat->driver.is_null() && w.alive(seat->driver)) instigator = seat->driver;
            const f32 amount = (speed - ram.min_speed + 1.0f) * ram.damage_per_ms;
            f.query<const Health3D, const LocalTransform>().each([&](Entity e, const Health3D& hp, const LocalTransform& ht) {
                if (e == car || e == instigator || hp.dead) return;
                if (hp.hits > 0 && hp.last_attacker == instigator && hp.since_hit < kRehitSeconds) return;
                const math::vec3 d = ht.position - lt.position;
                const f32 along = d.x * fwd.x + d.z * fwd.z;
                const f32 side = d.x * right.x + d.z * right.z;
                if (along < ram.zone_start || along > ram.zone_end || std::fabs(side) > ram.half_width) return;
                blows.push_back(Blow{e, car, instigator, amount, speed * ram.impulse_per_ms, ht.position, fwd});
            });
        });
    for (const Blow& b : blows) {
        if (!w.alive(b.target)) continue;
        DamageInfo d;
        d.amount = b.amount;
        d.instigator = b.instigator;
        d.point = b.point;
        d.direction = b.direction;
        d.impulse = b.impulse;
        apply_damage(w, b.target, d);
    }
}

}  // namespace

void install_vehicle_ram(World& world, asset::AssetSystem* assets) {
    install_combat3d(world, assets);
    install_vehicles3d(world, assets);
    if (world.try_resource<VehicleRamState>()) return;
    world.set_resource(VehicleRamState{});
    world.register_components<VehicleRam3D>();
    world.add_system(Phase::FixedUpdate_Pre, "kit_vehicle_ram", &kit_vehicle_ram);
}

}  // namespace mce::kit
