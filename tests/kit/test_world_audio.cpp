// World audio (M-OPENWORLD E3): EngineAudio follows the car's speed, WeaponAudio plays a shot per WeaponFired3D,
// FootstepAudio steps every stride while grounded and on foot, SirenAudio follows the wanted level; a world
// without audio is left untouched.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/asset/guid.h"
#include "mce/audio/components.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/seats3d.h"
#include "mce/kit/vehicles3d.h"
#include "mce/kit/wanted3d.h"
#include "mce/kit/world_audio.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f64 kDt = 1.0 / 60.0;
constexpr const char* kClip = "a0d10000-0000-4000-8000-0000000000b1";

struct Rig {
    World w;
    u64 t = 0;
    explicit Rig(bool audio = true) {
        w.register_components<LocalTransform, audio::AudioSource, Vehicle3D, VehicleSeat3D, CharacterController3D, Rider3D, Wanted3D>();
        w.register_event<WeaponFired3D>(Phase::FixedUpdate_Pre);
        if (audio) w.register_event<audio::PlaySfx>(Phase::FixedUpdate_Pre);  // what audio::install registers
        install_world_audio(w);
    }
    // Ticks n times and returns the PlaySfx events seen on the way.
    u32 run(u32 n, audio::PlaySfx* last = nullptr) {
        u32 plays = 0;
        for (u32 i = 0; i < n; ++i) {
            w.tick(kDt, t++);
            if (const auto* q = w.event_queue<audio::PlaySfx>())
                for (const audio::PlaySfx& ev : q->read()) {
                    ++plays;
                    if (last) *last = ev;
                }
        }
        return plays;
    }
};

asset::Guid clip() { return asset::parse_guid(kClip).value(); }

}  // namespace

TEST_CASE("world_audio: defaults and the engine's pitch and gain follow the speed") {
    Rig r;
    Vehicle3D car;
    VehicleSeat3D seat;
    audio::AudioSource src;
    const Entity e = r.w.spawn_with(car, seat, src, EngineAudio{});
    // Parked, nobody aboard: silent.
    r.run(1);
    CHECK(r.w.try_get<audio::AudioSource>(e)->gain_db == doctest::Approx(-60.0f));
    // A driver at a standstill: idle.
    r.w.try_get_mut<VehicleSeat3D>(e)->driver = r.w.spawn_with(LocalTransform{});
    r.run(1);
    CHECK(r.w.try_get<audio::AudioSource>(e)->gain_db == doctest::Approx(-12.0f));
    CHECK(r.w.try_get<audio::AudioSource>(e)->pick_rate == static_cast<u16>(std::lround(0.8f * 4096.0f)));
    // Half of top_speed: halfway in both.
    r.w.try_get_mut<Vehicle3D>(e)->speed = 14.0f;
    r.run(1);
    CHECK(r.w.try_get<audio::AudioSource>(e)->gain_db == doctest::Approx(-6.0f));
    CHECK(r.w.try_get<audio::AudioSource>(e)->pick_rate == static_cast<u16>(std::lround(1.5f * 4096.0f)));
    // Reversing counts as speed, and beyond top_speed the top holds.
    r.w.try_get_mut<Vehicle3D>(e)->speed = -80.0f;
    r.run(1);
    CHECK(r.w.try_get<audio::AudioSource>(e)->gain_db == doctest::Approx(0.0f));
    CHECK(r.w.try_get<audio::AudioSource>(e)->pick_rate == static_cast<u16>(std::lround(2.2f * 4096.0f)));
    // A car rolling with no driver entity (traffic) has its engine on.
    r.w.try_get_mut<VehicleSeat3D>(e)->driver = Entity{};
    r.w.try_get_mut<Vehicle3D>(e)->speed = 10.0f;
    r.run(1);
    CHECK(r.w.try_get<audio::AudioSource>(e)->gain_db > -12.0f);
}

TEST_CASE("world_audio: a shot plays at the muzzle for a weapon with WeaponAudio only") {
    Rig r;
    WeaponAudio wa;
    wa.shot.guid = clip();
    wa.gain_db = -3.0f;
    const Entity gun = r.w.spawn_with(wa);
    const Entity other = r.w.spawn_with(LocalTransform{});
    WeaponFired3D ev;
    ev.weapon = other;
    ev.origin = {1, 2, 3};
    EventAccess(r.w).push(ev);
    CHECK(r.run(3) == 0);  // events pushed between ticks arrive a tick later
    ev.weapon = gun;
    EventAccess(r.w).push(ev);
    audio::PlaySfx played;
    CHECK(r.run(3, &played) == 1);
    CHECK(played.clip == clip());
    CHECK(played.gain_db == doctest::Approx(-3.0f));
    CHECK(played.attenuation_id == 1);
    CHECK(played.position == math::vec3{1, 2, 3});
    CHECK(r.w.try_get<WeaponAudio>(gun)->fired == 1);
    CHECK(r.run(5) == 0);  // one event, one play
}

TEST_CASE("world_audio: footsteps come every stride while grounded and on foot") {
    Rig r;
    FootstepAudio fa;
    fa.clip.guid = clip();
    CharacterController3D cc;
    cc.grounded = true;
    cc.velocity = {0, 0, 5};
    const Entity e = r.w.spawn_with(LocalTransform{{4, 1, 0}, math::quat::identity(), {1, 1, 1}}, cc, fa);
    audio::PlaySfx played;
    CHECK(r.run(60, &played) == 2);  // 5 m/s over 1 s with a 1.8 m stride
    CHECK(played.position == math::vec3{4, 1, 0});
    CHECK(played.clip == clip());
    CHECK(r.w.try_get<FootstepAudio>(e)->steps == 2);
    r.w.try_get_mut<CharacterController3D>(e)->grounded = false;  // in the air
    CHECK(r.run(60) == 0);
    r.w.try_get_mut<CharacterController3D>(e)->grounded = true;
    r.w.try_get_mut<CharacterController3D>(e)->velocity = {0, 0, 0.2f};  // standing about
    CHECK(r.run(120) == 0);
    r.w.try_get_mut<CharacterController3D>(e)->velocity = {0, 0, 5};
    Rider3D rider;
    rider.seated = 1;
    r.w.add(e, rider);  // in a car
    CHECK(r.run(60) == 0);
}

TEST_CASE("world_audio: the siren slides with the wanted level") {
    Rig r;
    Wanted3D wanted;
    const Entity player = r.w.spawn_with(wanted);
    audio::AudioSource src;
    const Entity siren = r.w.spawn_with(src, SirenAudio{});
    r.run(10);
    CHECK(r.w.try_get<audio::AudioSource>(siren)->gain_db == doctest::Approx(-60.0f));
    r.w.try_get_mut<Wanted3D>(player)->level = 2;
    r.run(30);  // half a second at 30 dB/s
    const f32 half = r.w.try_get<audio::AudioSource>(siren)->gain_db;
    CHECK(half == doctest::Approx(-45.0f).epsilon(0.05));
    r.run(120);
    CHECK(r.w.try_get<audio::AudioSource>(siren)->gain_db == doctest::Approx(-6.0f));
    r.w.try_get_mut<Wanted3D>(player)->level = 0;
    r.run(180);
    CHECK(r.w.try_get<audio::AudioSource>(siren)->gain_db == doctest::Approx(-60.0f));
}

TEST_CASE("world_audio: a world without audio is left untouched") {
    Rig r(false);
    audio::AudioSource src;
    src.gain_db = 3.0f;
    const Entity e = r.w.spawn_with(Vehicle3D{}, VehicleSeat3D{}, src, EngineAudio{});
    r.run(5);
    CHECK(r.w.try_get<audio::AudioSource>(e)->gain_db == 3.0f);
    CHECK(r.w.try_get<audio::AudioSource>(e)->pick_rate == 0);
}
