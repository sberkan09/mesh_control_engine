#include "mce/kit/day_cycle.h"

#include "kit3d_internal.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/world_clock.h"

namespace mce::kit {
namespace {

struct DayCycleInstalled {};

struct Key {
    f32 hour;
    math::Color sun;
    f32 sun_i;
    math::Color sky, ground, fog;
    f32 fog_d, exp;
};

f32 lerp(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
math::Color lerp(const math::Color& a, const math::Color& b, f32 t) {
    return {lerp(a.r, b.r, t), lerp(a.g, b.g, t), lerp(a.b, b.b, t), 1.0f};
}

DayCycleValues blend(const Key& a, const Key& b, f32 t) {
    DayCycleValues v;
    v.sun = lerp(a.sun, b.sun, t);
    v.sun_intensity = lerp(a.sun_i, b.sun_i, t);
    v.sky = lerp(a.sky, b.sky, t);
    v.ground = lerp(a.ground, b.ground, t);
    v.fog = lerp(a.fog, b.fog, t);
    v.fog_density = lerp(a.fog_d, b.fog_d, t);
    v.exposure = lerp(a.exp, b.exp, t);
    return v;
}

}  // namespace

DayCycleValues evaluate_day_cycle(const DayCycle& c, f32 hour) {
    Key keys[kDayCycleKeys];
    keys[0] = {c.k0_hour, c.k0_sun, c.k0_sun_i, c.k0_sky, c.k0_ground, c.k0_fog, c.k0_fog_d, c.k0_exp};
    keys[1] = {c.k1_hour, c.k1_sun, c.k1_sun_i, c.k1_sky, c.k1_ground, c.k1_fog, c.k1_fog_d, c.k1_exp};
    keys[2] = {c.k2_hour, c.k2_sun, c.k2_sun_i, c.k2_sky, c.k2_ground, c.k2_fog, c.k2_fog_d, c.k2_exp};
    keys[3] = {c.k3_hour, c.k3_sun, c.k3_sun_i, c.k3_sky, c.k3_ground, c.k3_fog, c.k3_fog_d, c.k3_exp};
    keys[4] = {c.k4_hour, c.k4_sun, c.k4_sun_i, c.k4_sky, c.k4_ground, c.k4_fog, c.k4_fog_d, c.k4_exp};
    keys[5] = {c.k5_hour, c.k5_sun, c.k5_sun_i, c.k5_sky, c.k5_ground, c.k5_fog, c.k5_fog_d, c.k5_exp};
    keys[6] = {c.k6_hour, c.k6_sun, c.k6_sun_i, c.k6_sky, c.k6_ground, c.k6_fog, c.k6_fog_d, c.k6_exp};
    keys[7] = {c.k7_hour, c.k7_sun, c.k7_sun_i, c.k7_sky, c.k7_ground, c.k7_fog, c.k7_fog_d, c.k7_exp};
    u32 n = c.key_count < 1 ? 1 : (c.key_count > kDayCycleKeys ? kDayCycleKeys : c.key_count);
    for (u32 i = 1; i < n; ++i) {  // a stable insertion sort by hour
        const Key k = keys[i];
        u32 j = i;
        while (j > 0 && keys[j - 1].hour > k.hour) {
            keys[j] = keys[j - 1];
            --j;
        }
        keys[j] = k;
    }
    if (n == 1) return blend(keys[0], keys[0], 0.0f);
    f32 h = hour;
    if (!(h >= 0.0f)) h = 0.0f;
    if (h > 24.0f) h = 24.0f;
    // The segment [a, b] holding h; the closing one runs from the last key to the first, 24 h later.
    u32 ia = n - 1;
    for (u32 i = 0; i + 1 < n; ++i) {
        if (h >= keys[i].hour && h < keys[i + 1].hour) {
            ia = i;
            break;
        }
    }
    const u32 ib = ia + 1 < n ? ia + 1 : 0;
    f32 ha = keys[ia].hour;
    f32 hb = keys[ib].hour;
    if (ia == n - 1) {
        hb += 24.0f;
        if (h < ha) h += 24.0f;  // before the first key: the closing segment, a day later
    }
    const f32 span = hb - ha;
    f32 t = span > 0.0f ? (h - ha) / span : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return blend(keys[ia], keys[ib], t);
}

namespace {

void kit_day_cycle(Frame& f) {
    World& w = f.world();
    Environment3D* env = w.try_resource<Environment3D>();
    if (!env) return;
    bool first = true;  // one cycle per scene: the first in ECS order drives
    f.query<DayCycle>().each([&](Entity, DayCycle& c) {
        if (!first) return;
        first = false;
        if (!c.enabled || !(env->time_of_day >= 0.0f)) return;
        const DayCycleValues v = evaluate_day_cycle(c, env->time_of_day);
        env->sun_color = {v.sun.r, v.sun.g, v.sun.b};
        env->sun_intensity = v.sun_intensity;
        env->ambient_sky = {v.sky.r, v.sky.g, v.sky.b};
        env->ambient_ground = {v.ground.r, v.ground.g, v.ground.b};
        env->fog_color = {v.fog.r, v.fog.g, v.fog.b};
        env->fog_density = v.fog_density;
        env->exposure = v.exposure;
    });
}

}  // namespace

void install_day_cycle(World& world, asset::AssetSystem* assets) {
    install_world_clock(world, assets);  // it adds kit_world_clock, which this system follows
    if (world.try_resource<DayCycleInstalled>()) return;
    world.set_resource(DayCycleInstalled{});
    world.register_components<DayCycle>();
    world.add_system(Phase::FixedUpdate_Pre, "kit_day_cycle", &kit_day_cycle, "kit_world_clock");
}

}  // namespace mce::kit
