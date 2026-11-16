#include "mce/kit/drive_readout.h"

#include <cmath>

#include "mce/ecs/world.h"
#include "mce/kit/seats3d.h"
#include "mce/kit/vehicles3d.h"

namespace mce::kit {

static_assert(sizeof(DriveReadout3D) == 12);

namespace {

struct DriveReadoutInstalled {};

void kit_drive_readout(Frame& f) {
    World& w = f.world();
    f.query<DriveReadout3D>().each([&](Entity e, DriveReadout3D& d) {
        const Rider3D* r = w.try_get<Rider3D>(e);
        const Vehicle3D* v = r && r->seated == 1 && w.alive(r->vehicle) ? w.try_get<Vehicle3D>(r->vehicle) : nullptr;
        if (!v) {
            d.kmh = d.gear = d.rpm = kDriveReadoutOff;
            return;
        }
        d.kmh = std::fabs(v->speed) * 3.6f;
        d.gear = static_cast<f32>(v->gear);
        d.rpm = v->rpm;
    });
}

}  // namespace

void install_drive_readout(World& world) {
    if (world.try_resource<DriveReadoutInstalled>()) return;
    world.set_resource(DriveReadoutInstalled{});
    world.register_components<DriveReadout3D>();
    world.add_system(Phase::FixedUpdate_Pre, "kit_drive_readout", &kit_drive_readout, "kit3d_vehicles");
}

}  // namespace mce::kit
