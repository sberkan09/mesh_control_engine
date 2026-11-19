#include "mce/kit/drive_readout.h"

#include <cmath>

#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/seats3d.h"
#include "mce/kit/vehicle_extras.h"
#include "mce/kit/vehicles3d.h"

namespace mce::kit {

static_assert(sizeof(DriveReadout3D) == 20);

namespace {

struct DriveReadoutInstalled {};

void kit_drive_readout(Frame& f) {
    World& w = f.world();
    f.query<DriveReadout3D>().each([&](Entity e, DriveReadout3D& d) {
        const Rider3D* r = w.try_get<Rider3D>(e);
        const Vehicle3D* v = r && r->seated == 1 && w.alive(r->vehicle) ? w.try_get<Vehicle3D>(r->vehicle) : nullptr;
        if (!v) {
            d.kmh = d.gear = d.rpm = d.lights = d.hazard = kDriveReadoutOff;
            return;
        }
        d.kmh = std::fabs(v->speed) * 3.6f;
        d.gear = static_cast<f32>(v->gear);
        d.rpm = v->rpm;
        const SpotLight3D* sl = w.try_get<SpotLight3D>(r->vehicle);
        const VehicleLights3D* vl = w.try_get<VehicleLights3D>(r->vehicle);
        const bool lit = sl && sl->enabled && sl->intensity > 0.0f;
        const VehicleHazards3D* hz = w.try_get<VehicleHazards3D>(r->vehicle);
        d.hazard = hz && hz->on ? 1.0f : 0.0f;
        d.lights = !lit ? 0.0f : vl && vl->mode == HeadlightMode::High ? 2.0f : 1.0f;
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
