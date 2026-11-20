#include "mce/kit/vehicle_extras.h"

#include "mce/audio/components.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/input/actions.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/seats3d.h"
#include "mce/kit/timed_light.h"
#include "mce/kit/vehicles3d.h"
#include "mce/kit/world_audio.h"
#include "mce/math/math.h"
#include "mce/string_id.h"

#include <cmath>

namespace mce::kit {

static_assert(sizeof(VehicleHorn3D) == 48);
static_assert(sizeof(VehicleLights3D) == 20);
static_assert(sizeof(BrakeLight3D) == 52);
static_assert(sizeof(LightFlasher3D) == 48);
static_assert(sizeof(TurnSignal3D) == 36);
static_assert(sizeof(VehicleHazards3D) == 8);

namespace {

struct VehicleExtrasInstalled {};

constexpr StringId kHorn{"Horn"};
constexpr StringId kLights{"Lights"};
constexpr StringId kHazard{"Hazard"};

// The driver, when it is a seated rider that reads input (the player; traffic and police cars have none).
bool player_drives(World& w, Entity car) {
    const VehicleSeat3D* seat = w.try_get<VehicleSeat3D>(car);
    if (!seat || seat->driver.is_null() || !w.alive(seat->driver)) return false;
    const Rider3D* r = w.try_get<Rider3D>(seat->driver);
    return r && r->use_input == 1 && r->seated == 1 && r->vehicle == car;
}

// True while the car brakes: the same inputs kit3d_vehicles feeds the wheels (its own brake and handbrake, the player's brake
// trigger and handbrake key, the throttle against the way it rolls).
bool car_braking(const Vehicle3D& v, const input::InputState* in) {
    f32 throttle = v.throttle, brake = v.brake, handbrake = v.handbrake;
    if (v.use_input && in) {
        throttle = in->move.y;
        brake = math::max(brake, in->trigger_l);
        if (in->jump_held) handbrake = 1.0f;
    }
    if (throttle * v.speed < 0.0f && std::fabs(v.speed) > 1.0f) return true;
    return brake > 0.05f || handbrake > 0.05f;
}

void kit_vehicle_extras(Frame& f) {
    World& w = f.world();
    const input::InputState* in = w.try_resource<input::InputState>();
    const bool audio = w.event_queue<audio::PlaySfx>() != nullptr;

    f.query<VehicleLights3D>().each([&](Entity e, VehicleLights3D& l) {
        if (in && in->has_action(kLights) && in->pressed(kLights) && player_drives(w, e)) {
            l.mode = l.mode == HeadlightMode::Auto   ? HeadlightMode::On
                     : l.mode == HeadlightMode::On   ? HeadlightMode::High
                     : l.mode == HeadlightMode::High ? HeadlightMode::Off
                                                     : HeadlightMode::Auto;
        }
        SpotLight3D* sl = w.try_get_mut<SpotLight3D>(e);
        if (!sl) return;
        const VehicleHorn3D* hn = l.flash_s > 0.0f ? w.try_get<VehicleHorn3D>(e) : nullptr;
        const bool flash = hn && hn->since_s < l.flash_s;  // M-HORNFLASH: a honk flashes the beam
        const bool high = l.mode == HeadlightMode::High || flash;
        if (!high && l.base_radius > 0.0f) {  // leaving High: give the spot back
            sl->radius = l.base_radius;
            sl->intensity = l.base_intensity;
            l.base_radius = 0.0f;
            if (l.mode == HeadlightMode::Auto) sl->enabled = l.base_on;  // a flash in Auto: the lamp goes back as it was
        }
        if (l.mode == HeadlightMode::Auto && !flash) return;
        if (l.mode == HeadlightMode::Off && !flash) {
            sl->enabled = 0;
            return;
        }
        const TimedLight3D* t = w.try_get<TimedLight3D>(e);
        if (t && t->captured) sl->intensity = t->spot_scale;
        if (high && l.base_radius <= 0.0f) l.base_on = static_cast<u8>(sl->enabled);
        sl->enabled = 1;
        if (high) {
            if (l.base_radius <= 0.0f) {  // entering High: remember the normal beam
                l.base_radius = sl->radius;
                l.base_intensity = sl->intensity;
            }
            sl->radius = l.base_radius * l.high_scale;
            sl->intensity = l.base_intensity * l.high_scale;
        }
    });

    f.query<BrakeLight3D, PointLight3D>().each([&](Entity e, BrakeLight3D& b, PointLight3D& pl) {
        const ChildOf* c = w.try_get<ChildOf>(e);
        const Vehicle3D* v = c ? w.try_get<Vehicle3D>(c->parent) : nullptr;
        const bool braking = v && car_braking(*v, in);
        const bool reversing = v && !braking && b.reverse > 0.0f && v->gear < 0;
        b.lit = braking ? 1 : reversing ? 2 : 0;
        const SpotLight3D* head = c && b.headlight_idle > 0.0f ? w.try_get<SpotLight3D>(c->parent) : nullptr;
        const bool headlights = head && head->enabled && head->intensity > 0.0f;
        const f32 level = braking ? b.braking : reversing ? b.reverse : headlights ? b.headlight_idle : b.idle;
        pl.color = reversing ? b.reverse_color : b.brake_color;
        pl.intensity = level;
        pl.enabled = level > 0.0f ? 1u : 0u;
    });

    f.query<VehicleHazards3D>().each([&](Entity e, VehicleHazards3D& h) {
        if (in && in->has_action(kHazard) && in->pressed(kHazard) && player_drives(w, e)) h.on = h.on ? 0 : 1;
        if (h.crash_speed > 0.0f)  // a hard hit switches them on (M-CRASHHAZARD)
            for (const ContactBegin3D& cb : f.events().read<ContactBegin3D>())
                if ((cb.a == e || cb.b == e) && cb.approach_speed >= h.crash_speed) h.on = 1;
    });

    f.query<TurnSignal3D, PointLight3D>().each([&](Entity e, TurnSignal3D& ts, PointLight3D& pl) {
        const ChildOf* c = w.try_get<ChildOf>(e);
        const Vehicle3D* v = c ? w.try_get<Vehicle3D>(c->parent) : nullptr;
        f32 steer = v ? v->steer : 0.0f;
        if (v && v->use_input && in && std::fabs(in->move.x) > std::fabs(steer)) steer = in->move.x;
        const VehicleHazards3D* hz = c ? w.try_get<VehicleHazards3D>(c->parent) : nullptr;
        const bool hazard = v && hz && hz->on;
        const bool active = hazard || (v && std::fabs(v->speed) > 0.5f && steer * static_cast<f32>(ts.side) > ts.threshold);
        if (!active) {
            ts.t = 0.0f;
            pl.enabled = 0;
            return;
        }
        ts.t += f.dt;
        const bool on = std::fmod(ts.t, ts.period_s) < ts.period_s * 0.5f;
        pl.color = ts.color;
        pl.intensity = ts.intensity;
        pl.enabled = on ? 1u : 0u;
    });

    f.query<LightFlasher3D, PointLight3D>().each([&](Entity e, LightFlasher3D& fl, PointLight3D& pl) {
        if (fl.follow_siren) {
            const ChildOf* co = w.try_get<ChildOf>(e);
            const SirenAudio* sa = co && w.alive(co->parent) ? w.try_get<SirenAudio>(co->parent) : nullptr;
            if (!sa || sa->current_db < -30.0f) {  // the siren is silent: the bar is dark and starts over with A
                fl.t = 0.0f;
                pl.enabled = 0;
                return;
            }
        }
        fl.t += f.dt;
        const f32 half = fl.period_s * 0.5f;
        const f32 phase = std::fmod(fl.t, fl.period_s);
        const bool a = phase < half;
        const f32 into = (a ? phase : phase - half) / half;  // 0..1 through this half
        pl.color = a ? fl.color_a : fl.color_b;
        pl.intensity = fl.intensity * (1.0f - 0.7f * into);
        pl.enabled = 1;
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
    world.register_components<VehicleHorn3D, VehicleLights3D, BrakeLight3D, LightFlasher3D, TurnSignal3D,
                              VehicleHazards3D>();
    world.add_system(Phase::FixedUpdate_Pre, "kit_vehicle_extras", &kit_vehicle_extras, "kit_timed_light");
}

}  // namespace mce::kit
