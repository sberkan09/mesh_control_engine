#include "mce/kit/world_audio.h"

#include <cmath>

#include "mce/audio/audio.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/seats3d.h"
#include "mce/kit/vehicles3d.h"
#include "mce/kit/wanted3d.h"

namespace mce::kit {
namespace {

constexpr f32 kFloorDb = -60.0f;
constexpr f32 kRunningSpeed = 0.5f;  // m/s: a car this fast has its engine on whoever sits in it (traffic has no driver entity)

f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
f32 finite(f32 v, f32 fallback) { return std::isfinite(v) ? v : fallback; }
f32 clamp_db(f32 v) { return math::clamp(finite(v, 0.0f), kFloorDb, 12.0f); }

// AudioSource::pick_rate is the voice's own pitch, 4.12 fixed point (0 = 1).
u16 rate_of(f32 pitch) { return pitch == 1.0f ? u16(0) : static_cast<u16>(std::lround(math::clamp(pitch, audio::kMinPitch, audio::kMaxPitch) * 4096.0f)); }

void play(Frame& f, const asset::Guid& clip, f32 gain_db, u16 attenuation_id, math::vec3 position) {
    audio::PlaySfx ev;
    ev.clip = clip;
    ev.gain_db = clamp_db(gain_db);
    ev.attenuation_id = attenuation_id;
    ev.position = position;
    f.events().push(ev);
}

void engines(Frame& f, audio::AudioDevice* device) {
    World& w = f.world();
    f.query<EngineAudio&, audio::AudioSource&, const Vehicle3D&>().each([&](Entity e, EngineAudio& ea, audio::AudioSource& src, const Vehicle3D& car) {
        const VehicleSeat3D* seat = w.try_get<VehicleSeat3D>(e);
        const f32 speed = std::fabs(finite(car.speed, 0.0f));
        const bool running = speed > kRunningSpeed || (seat && !seat->driver.is_null());
        const f32 t = math::clamp(speed / math::max(finite(ea.top_speed, 28.0f), 1.0f), 0.0f, 1.0f);
        const f32 pitch = math::clamp(lerp(finite(ea.idle_pitch, 1.0f), finite(ea.top_pitch, 1.0f), t), audio::kMinPitch, audio::kMaxPitch);
        src.gain_db = running ? clamp_db(lerp(ea.idle_db, ea.top_db, t)) : clamp_db(ea.parked_db);
        src.pick_rate = rate_of(pitch);
        if (device && !src.handle.is_null() && std::fabs(pitch - ea.applied_pitch) > 0.002f) {
            device->set_handle_pitch(src.handle, pitch);
            ea.applied_pitch = pitch;
        }
    });
}

void shots(Frame& f) {
    World& w = f.world();
    for (const WeaponFired3D& ev : f.events().read<WeaponFired3D>()) {
        WeaponAudio* wa = w.alive(ev.weapon) ? w.try_get_mut<WeaponAudio>(ev.weapon) : nullptr;
        if (!wa || wa->shot.guid.is_null()) continue;
        play(f, wa->shot.guid, wa->gain_db, wa->attenuation_id, ev.origin);
        ++wa->fired;
    }
}

void footsteps(Frame& f) {
    World& w = f.world();
    const f32 dt = static_cast<f32>(f.dt);
    f.query<FootstepAudio&, const CharacterController3D&, const LocalTransform&>().each(
        [&](Entity e, FootstepAudio& fa, const CharacterController3D& cc, const LocalTransform& t) {
            const Rider3D* rider = w.try_get<Rider3D>(e);
            if (!cc.grounded || (rider && rider->seated)) {
                fa.travelled = 0.0f;
                return;
            }
            const f32 speed = std::sqrt(cc.velocity.x * cc.velocity.x + cc.velocity.z * cc.velocity.z);
            if (!(speed > 0.5f)) return;
            fa.travelled += speed * dt;
            const f32 stride = math::max(finite(fa.stride, 1.8f), 0.2f);
            if (fa.travelled < stride) return;
            fa.travelled -= stride;
            ++fa.steps;
            if (!fa.clip.guid.is_null()) play(f, fa.clip.guid, fa.gain_db, fa.attenuation_id, t.position);
        });
}

void sirens(Frame& f) {
    World& w = f.world();
    u8 level = 0;
    bool found = false;
    w.query<const Wanted3D&>().each([&](Entity, const Wanted3D& wanted) {
        if (found) return;
        found = true;
        level = wanted.level;
    });
    const f32 dt = static_cast<f32>(f.dt);
    f.query<SirenAudio&, audio::AudioSource&>().each([&](Entity, SirenAudio& sa, audio::AudioSource& src) {
        const f32 target = level >= sa.min_level && level > 0 ? clamp_db(sa.active_db) : kFloorDb;
        const f32 step = math::max(finite(sa.slew_db_per_s, 30.0f), 1.0f) * dt;
        const f32 now = math::clamp(finite(sa.current_db, kFloorDb), kFloorDb, 12.0f);
        sa.current_db = target > now ? math::min(target, now + step) : math::max(target, now - step);
        src.gain_db = sa.current_db;
    });
}

void kit_world_audio(Frame& f) {
    World& w = f.world();
    const audio::AudioRef* ref = w.try_resource<audio::AudioRef>();
    if (!w.event_queue<audio::PlaySfx>() && !ref) return;  // no audio in this world
    engines(f, ref ? ref->device : nullptr);
    shots(f);
    footsteps(f);
    sirens(f);
}

}  // namespace

void install_world_audio(World& world) {
    world.register_components<EngineAudio, WeaponAudio, FootstepAudio, SirenAudio>();
    world.add_system(Phase::FixedUpdate_Post, "kit_world_audio", &kit_world_audio);
}

}  // namespace mce::kit
