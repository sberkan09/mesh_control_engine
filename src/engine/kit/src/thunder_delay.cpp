#include "mce/kit/thunder_delay.h"

#include <cmath>

#include "mce/audio/components.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/precipitation.h"

namespace mce::kit {
namespace {

struct ThunderInstalled {};

constexpr f64 kNominalSpeed = 343.0;  // what a LightningStruck delay was computed at
constexpr f64 kRefDistance = 100.0;   // metres: linear gain is 1 / (1 + d / this)
constexpr f32 kFloorDb = -60.0f;
constexpr u64 kMaxTicks = 1ull << 31;

f32 clamp_speed(f32 v) {
    if (std::isnan(v)) return 343.0f;
    return v < 1.0f ? 1.0f : (v > 1.0e6f ? 1.0e6f : v);
}
f32 clamp_volume(f32 v) {
    if (std::isnan(v)) return 1.0f;
    return v < 0.0f ? 0.0f : (v > 4.0f ? 4.0f : v);
}

void kit_thunder_delay(Frame& f) {
    World& w = f.world();
    ThunderDelay* td = nullptr;
    w.query<ThunderDelay>().each([&](Entity, ThunderDelay& c) {
        if (!td) td = &c;
    });
    if (!td) return;
    if (td->pending > kThunderQueue) td->pending = kThunderQueue;  // a hand-edited count

    const bool audio = w.event_queue<audio::PlaySfx>() != nullptr;
    auto play = [&](f32 gain_db) {
        if (!audio || td->clip.guid == asset::Guid{}) return;
        audio::PlaySfx ev;
        ev.clip = td->clip.guid;
        ev.gain_db = gain_db;
        f.events().push(ev);
    };

    // Strikes from earlier ticks count down; the ones that reach 0 sound now, in order. Slots left behind are zeroed.
    u32 keep = 0;
    for (u32 i = 0; i < td->pending; ++i) {
        const u32 due = td->due[i] > 0 ? td->due[i] - 1 : 0;
        if (due == 0) {
            play(td->gain_db[i]);
        } else {
            td->due[keep] = due;
            td->gain_db[keep] = td->gain_db[i];
            ++keep;
        }
    }
    for (u32 i = keep; i < kThunderQueue; ++i) {
        td->due[i] = 0;
        td->gain_db[i] = 0.0f;
    }
    td->pending = keep;

    bool listener = false;
    w.query<const Camera3DComponent&>().each([&](const Camera3DComponent&) { listener = true; });
    if (!listener || td->clip.guid == asset::Guid{}) return;

    const f64 dt = static_cast<f64>(f.dt);
    const f64 speed = static_cast<f64>(clamp_speed(td->speed_mps));
    const f64 volume = static_cast<f64>(clamp_volume(td->volume));
    for (const LightningStruck& s : f.events().read<LightningStruck>()) {
        // A positioned strike (LightningBolt) carries its real distance; otherwise the delay at the nominal speed.
        const f64 d = s.has_pos ? static_cast<f64>(s.distance_m) : static_cast<f64>(s.thunder_delay_ticks) * dt * kNominalSpeed;
        u64 n = 0;
        if (dt > 0.0) {
            const f64 x = d / speed / dt - 1e-6;
            n = x <= 0.0 ? 0 : (x >= static_cast<f64>(kMaxTicks) ? kMaxTicks : static_cast<u64>(std::ceil(x)));
        }
        const f64 linear = volume / (1.0 + d / kRefDistance);
        const f32 db = linear > 0.001 ? static_cast<f32>(20.0 * std::log10(linear)) : kFloorDb;
        const f32 gain = db < kFloorDb ? kFloorDb : db;
        // The queue swaps after FixedUpdate_Pre, so this system reads a strike one tick after it was pushed: n - 1
        // ticks remain (n of 0 or 1 sounds right away).
        n = n > 0 ? n - 1 : 0;
        if (n == 0) {
            play(gain);
        } else if (td->pending < kThunderQueue) {  // full: the new strike is dropped
            td->due[td->pending] = static_cast<u32>(n);
            td->gain_db[td->pending] = gain;
            ++td->pending;
        }
    }
}

}  // namespace

void install_thunder_delay(World& world) {
    if (world.try_resource<ThunderInstalled>()) return;
    world.set_resource(ThunderInstalled{});
    world.add_system(Phase::FixedUpdate_Pre, "kit_thunder_delay", &kit_thunder_delay, "kit_lightning");
}

}  // namespace mce::kit
