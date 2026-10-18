#include "mce/kit/world_clock.h"

#include <cmath>

#include "kit3d_internal.h"
#include "mce/ecs/world.h"
#include "mce/kit/combat3d.h"
#include "mce/kit/kit3d.h"

namespace mce::kit {
namespace {

struct WorldClockInstalled {};

constexpr u64 kSub = 1024;  // sub-steps per tick at time_scale 1

// a * b / c without a wider type: the remainder (< c) times b stays far below 2^64 because c is a day's worth of
// sub-steps (a day length of years still gives c < 2^40) and b is at most c.
u64 mul_div(u64 a, u64 b, u64 c) { return (a / c) * b + (a % c) * b / c; }

// The hour 0..24 of `phase` out of `den`.
f32 hour_of(u64 phase, u64 den) {
    const f64 h = 24.0 * static_cast<f64>(phase) / static_cast<f64>(den);
    return static_cast<f32>(h < 23.999998 ? h : 23.999998);
}

void tick_clock(World& w, Frame& f, Entity e, WorldClock& c) {
    const f64 dt = static_cast<f64>(f.dt);
    const f64 len = static_cast<f64>(c.day_length_seconds);
    u64 day_ticks = dt > 0.0 && len > 0.0 ? static_cast<u64>(std::llround(len / dt)) : 0;
    if (day_ticks < 1) day_ticks = 1;
    const u64 den = day_ticks * kSub;

    if (!c.started || c.hour != c.hour_seen || c.phase_den == 0) {  // the start value, or an edit of `hour`
        const f64 h = static_cast<f64>(c.hour < 0.0f ? 0.0f : (c.hour > 24.0f ? 24.0f : c.hour));
        c.phase = static_cast<u64>(std::llround(h / 24.0 * static_cast<f64>(den)));
        if (c.phase >= den) c.phase = 0;
        c.started = 1;
    } else if (c.phase_den != den) {  // the day length changed: keep the time of day
        c.phase = mul_div(c.phase, den, c.phase_den);
    }
    c.phase_den = den;

    f32 scale = c.time_scale;
    if (!(scale > 0.0f)) scale = 0.0f;  // negative and NaN stop the clock
    if (scale > 100.0f) scale = 100.0f;
    const u64 step = c.paused ? 0 : static_cast<u64>(std::llround(static_cast<f64>(scale) * static_cast<f64>(kSub)));
    if (step > 0) {
        const u64 total = c.phase + step;
        const u64 h_old = mul_div(c.phase, 24, den);
        const u64 h_new = mul_div(total, 24, den);
        for (u64 h = h_old + 1; h <= h_new; ++h) {
            f.events().push(HourPassed{static_cast<u32>(h % 24)});
            if (h % 24 == 0) {
                ++c.day;
                f.events().push(DayPassed{c.day});
            }
        }
        c.phase = total % den;
    }
    c.hour = hour_of(c.phase, den);
    c.hour_seen = c.hour;
    if (c.drive_environment) {
        if (Environment3D* env = w.try_resource<Environment3D>()) env->time_of_day = c.hour;
    }
    (void)e;
}

void kit_world_clock(Frame& f) {
    World& w = f.world();
    bool first = true;  // one clock per scene: the first in ECS order drives
    f.query<WorldClock>().each([&](Entity e, WorldClock& c) {
        if (!first) return;
        first = false;
        tick_clock(w, f, e, c);
    });
}

}  // namespace

void install_world_clock(World& world, asset::AssetSystem* assets) {
    install_combat3d(world, assets);  // it adds kit3d_health, which this system follows
    if (world.try_resource<WorldClockInstalled>()) return;
    world.set_resource(WorldClockInstalled{});
    world.register_components<WorldClock>();
    world.register_event<HourPassed>(Phase::FixedUpdate_Pre);
    world.register_event<DayPassed>(Phase::FixedUpdate_Pre);
    world.add_system(Phase::FixedUpdate_Pre, "kit_world_clock", &kit_world_clock, "kit3d_health");
}

}  // namespace mce::kit
