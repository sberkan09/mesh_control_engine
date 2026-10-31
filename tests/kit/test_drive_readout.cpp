// Drive readout (M-DRIVEHUD): DriveReadout3D shows the speed (km/h), gear and rpm of the car the rider sits in, and
// reads -1000 on foot so a HUD range hides it.

#include <doctest/doctest.h>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/drive_readout.h"
#include "mce/kit/seats3d.h"
#include "mce/kit/vehicles3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

// Stands in for the vehicle kit: the readout runs after a system of this name and only reads Vehicle3D.
void kit3d_vehicles_stub(Frame& f) {
    f.query<Vehicle3D>().each([](Entity, Vehicle3D& v) {
        v.speed = -10.0f;
        v.gear = -1;
        v.rpm = 2500.0f;
    });
}

struct Scene {
    World w;
    u64 t = 0;
    Entity car, rider;
    Scene() {
        w.register_components<LocalTransform>();
        w.register_components<Vehicle3D, Rider3D>();
        w.add_system(Phase::FixedUpdate_Pre, "kit3d_vehicles", &kit3d_vehicles_stub);
        install_drive_readout(w);
        Vehicle3D v;  
        v.speed = -10.0f;
        v.gear = -1;
        v.rpm = 2500.0f;
        car = w.spawn_with(LocalTransform{}, v);
        rider = w.spawn_with(LocalTransform{}, Rider3D{}, DriveReadout3D{});
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    const DriveReadout3D& d() { return *w.try_get<DriveReadout3D>(rider); }
    void sit(bool seated) {
        Rider3D* r = w.try_get_mut<Rider3D>(rider);
        r->seated = seated ? 1 : 0;
        r->vehicle = seated ? car : Entity::null;
    }
};

}  // namespace

TEST_CASE("drive_readout: on foot every field reads off, seated it shows the car in km/h") {
    Scene s;
    s.run(2);
    CHECK(s.d().kmh == doctest::Approx(kDriveReadoutOff));
    CHECK(s.d().gear == doctest::Approx(kDriveReadoutOff));
    s.sit(true);
    s.run(2);
    CHECK(s.d().kmh == doctest::Approx(36.0f));  // 10 m/s reversing
    CHECK(s.d().gear == doctest::Approx(-1.0f));
    CHECK(s.d().rpm == doctest::Approx(2500.0f));
    s.sit(false);
    s.run(2);
    CHECK(s.d().kmh == doctest::Approx(kDriveReadoutOff));
}
