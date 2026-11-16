#include "mce/kit/timed_light.h"

#include <cmath>

#include "kit3d_internal.h"
#include "mce/ecs/world.h"
#include "mce/kit/day_cycle.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/world_clock.h"

namespace mce::kit {
namespace {

struct TimedLightInstalled {};

f32 wrap24(f32 h) {
    h = std::fmod(h, 24.0f);
    if (h < 0.0f) h += 24.0f;
    return h;
}

// Did the clock pass hour `h` going from `prev` to `cur` (prev < h <= cur, wrapping midnight)?
bool crossed(f32 prev, f32 cur, f32 h) {
    if (cur >= prev) return prev < h && h <= cur;
    return h > prev || h <= cur;
}

}  // namespace

f32 timed_light_ramp(const TimedLight3D& t, f32 hour) {
    const f32 on = wrap24(t.on_hour);
    const f32 off = wrap24(t.off_hour);
    if (on == off) return 1.0f;
    const f32 fade_h = (t.fade_minutes > 0.0f ? t.fade_minutes : 0.0f) / 60.0f;
    const f32 since_on = wrap24(hour - on);
    const f32 window = wrap24(off - on);
    if (since_on < window) return fade_h > 0.0f ? std::fmin(1.0f, since_on / fade_h) : 1.0f;
    const f32 at_off = fade_h > 0.0f ? std::fmin(1.0f, window / fade_h) : 1.0f;
    const f32 since_off = since_on - window;
    if (!(fade_h > 0.0f) || since_off >= fade_h) return 0.0f;
    return at_off * (1.0f - since_off / fade_h);
}

namespace {

void kit_timed_light(Frame& f) {
    World& w = f.world();
    f32 hour = -1.0f;
    bool have_clock = false;
    f.query<WorldClock>().each([&](Entity, WorldClock& c) {
        if (have_clock) return;
        have_clock = true;
        hour = c.hour;
    });
    bool first = true;
    f.query<TimedLight3D>().each([&](Entity e, TimedLight3D& t) {
        PointLight3D* pl = w.try_get_mut<PointLight3D>(e);
        SpotLight3D* sl = w.try_get_mut<SpotLight3D>(e);
        if (!t.captured) {
            if (pl) t.light_scale = pl->intensity;
            if (sl) t.spot_scale = sl->intensity;
            t.captured = 1;
        }
        const bool lead = first;
        first = false;
        if (!have_clock) return;
        const f32 ramp = timed_light_ramp(t, hour);
        if (pl) {
            pl->intensity = t.light_scale * ramp;
            pl->enabled = ramp > 0.0f ? 1u : 0u;
        }
        if (sl) {
            sl->intensity = t.spot_scale * ramp;
            sl->enabled = ramp > 0.0f ? 1u : 0u;
        }
        if (MeshRef* m = w.try_get_mut<MeshRef>(e)) {
            const f32 k = t.emissive_off + (t.emissive_on - t.emissive_off) * ramp;
            m->emissive_tint = {k, k, k};
        }
        if (lead) {
            const f32 prev = t.last_hour;
            t.last_hour = hour;
            if (prev >= 0.0f && wrap24(hour - prev) <= 6.0f) {
                if (crossed(prev, hour, wrap24(t.on_hour))) f.events().push(DuskStarted{t.on_hour});
                if (crossed(prev, hour, wrap24(t.off_hour))) f.events().push(DawnStarted{t.off_hour});
            }
        }
    });
}

}  // namespace

void install_timed_light(World& world, asset::AssetSystem* assets) {
    install_day_cycle(world, assets);  // it adds kit_day_cycle, which this system follows
    if (world.try_resource<TimedLightInstalled>()) return;
    world.set_resource(TimedLightInstalled{});
    world.register_components<TimedLight3D>();
    world.register_event<DuskStarted>(Phase::FixedUpdate_Pre);
    world.register_event<DawnStarted>(Phase::FixedUpdate_Pre);
    world.add_system(Phase::FixedUpdate_Pre, "kit_timed_light", &kit_timed_light, "kit_day_cycle");
}

}  // namespace mce::kit
