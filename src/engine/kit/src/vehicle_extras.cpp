#include "mce/kit/vehicle_extras.h"

#include "mce/audio/components.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/seats3d.h"
#include "mce/kit/timed_light.h"
#include "mce/string_id.h"

namespace mce::kit {

static_assert(sizeof(VehicleHorn3D) == 48);
static_assert(sizeof(VehicleLights3D) == 4);

namespace {

struct VehicleExtrasInstalled {};

constexpr StringId kHorn{"Horn"};
constexpr StringId kLights{"Lights"};

// The driver, when it is a seated rider that reads input (the player; traffic and police cars have none).
bool player_drives(World& w, Entity car) {
    const VehicleSeat3D* seat = w.try_get<VehicleSeat3D>(car);
    if (!seat || seat->driver.is_null() || !w.alive(seat->driver)) return false;
    const Rider3D* r = w.try_get<Rider3D>(seat->driver);
    return r && r->use_input == 1 && r->seated == 1 && r->vehicle == car;
}

void kit_vehicle_extras(Frame& f) {
    World& w = f.world();
    const input::InputState* in = w.try_resource<input::InputState>();
    const bool audio = w.event_queue<audio::PlaySfx>() != nullptr;

    f.query<VehicleLights3D>().each([&](Entity e, VehicleLights3D& l) {
        if (in && in->has_action(kLights) && in->pressed(kLights) && player_drives(w, e)) {
            l.mode = l.mode == HeadlightMode::Auto ? HeadlightMode::On
                     : l.mode == HeadlightMode::On ? HeadlightMode::Off
                                                   : HeadlightMode::Auto;
        }
        if (l.mode == HeadlightMode::Auto) return;
        SpotLight3D* sl = w.try_get_mut<SpotLight3D>(e);
        if (!sl) return;
        if (l.mode == HeadlightMode::Off) {
            sl->enabled = 0;
            return;
        }
        const TimedLight3D* t = w.try_get<TimedLight3D>(e);
        if (t && t->captured) sl->intensity = t->spot_scale;
        sl->enabled = 1;
    });

    f.query<VehicleHorn3D>().each([&](Entity e, VehicleHorn3D& h) {
        if (h.since_s < 1.0e6f) h.since_s += f.dt;
        if (in && in->has_action(kHorn) && in->pressed(kHorn) && player_drives(w, e)) h.honk = 1;
        if (!h.honk) return;
        h.honk = 0;
        if (h.since_s < h.min_gap_s) return;
        h.since_s = 0.0f;
        ++h.honks;
        if (!audio || h.clip.guid == asset::Guid{}) return;
        audio::PlaySfx ev;
        ev.clip = h.clip.guid;
        ev.gain_db = h.gain_db;
        ev.attenuation_id = h.attenuation_id;
        if (const LocalTransform* lt = w.try_get<LocalTransform>(e)) ev.position = lt->position;
        f.events().push(ev);
    });
}

}  // namespace

void install_vehicle_extras(World& world, asset::AssetSystem* assets) {
    install_timed_light(world, assets);  // the light write this system overrides
    if (world.try_resource<VehicleExtrasInstalled>()) return;
    world.set_resource(VehicleExtrasInstalled{});
    world.register_components<VehicleHorn3D, VehicleLights3D>();
    world.add_system(Phase::FixedUpdate_Pre, "kit_vehicle_extras", &kit_vehicle_extras, "kit_timed_light");
}

}  // namespace mce::kit
