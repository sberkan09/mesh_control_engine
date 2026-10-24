// A slice of the open-world loop across modules: a driver runs a civilian over, police see it, the wanted level rises and the
// unit gives chase (vehicle_ram + combat3d + wanted3d together).

#include <doctest/doctest.h>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/seats3d.h"
#include "mce/kit/vehicle_ram.h"
#include "mce/kit/vehicles3d.h"
#include "mce/kit/wanted3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

LocalTransform at(f32 x, f32 y, f32 z) { return LocalTransform{{x, y, z}, math::quat::identity(), {1, 1, 1}}; }

}  // namespace

TEST_CASE("gta slice: running a civilian over in front of police raises the wanted level and starts a chase") {
    World w;
    w.register_components<LocalTransform>();
    install_wanted3d(w);
    install_vehicle_ram(w, nullptr);
    BoxCollider3D ground;
    ground.half = {300.0f, 0.5f, 300.0f};
    w.spawn_with(at(0, -0.5f, 0), ground);

    Health3D ph;
    ph.team = 1;
    const Entity player = w.spawn_with(at(0, 1.0f, 0), Wanted3D{}, ph);
    VehicleSeat3D seat;
    seat.driver = player;
    Vehicle3D v;
    v.speed = 12.0f;
    const Entity car = w.spawn_with(at(0, 0.5f, 0), v, VehicleRam3D{}, seat);  // forward is -Z
    Health3D ch;
    ch.team = 3;
    const Entity civilian = w.spawn_with(at(0, 1.0f, -2.4f), ch);
    Health3D cph;
    cph.team = 2;
    const Entity cop = w.spawn_with(at(5, 1.0f, 20), Police3D{}, cph);

    u32 crimes = 0;
    u64 t = 0;
    for (u32 i = 0; i < 240; ++i) {
        if (i < 3) w.try_get_mut<Vehicle3D>(car)->speed = 12.0f;  // the vehicle system zeroes speed on a car with no body
        w.tick(kDt, t++);
        crimes += static_cast<u32>(EventAccess(w).read<Crime3D>().size());
    }
    CHECK(w.try_get<Health3D>(civilian)->current < ch.max);
    CHECK(w.try_get<Health3D>(civilian)->last_attacker == player);
    CHECK(crimes >= 1);
    CHECK(w.try_get<Wanted3D>(player)->level >= 1);
    CHECK(w.try_get<Police3D>(cop)->state != PoliceState::Idle);
}
