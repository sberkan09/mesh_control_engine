#pragma once
// Bird flocks at dawn and dusk as data (M-BIRDFLOCK step 26a). One optional Flock per scene (the first in ECS order is
// read). install_weather registers the component; there is no system and nothing is stored between ticks.
//
//   Flock    Authoring (all Save, clamped to the Range when read and never rewritten): `enabled` (1); `migration_full`
//            (0.06, 0..0.5) and `migration_end` (0.12, 0..0.5) the half-widths, in season phase, of the migration
//            trapezoid around the spring centre 0.125 and the autumn centre 0.625 (full strength inside migration_full,
//            nothing beyond migration_end, a smoothstep between; an inverted or empty pair falls back to the defaults);
//            `manual` (0, 0..1) a floor for the migration, so a scene can show birds all year; `twilight_width` (0.25,
//            0.01..1) the sun height |sun_y| below which the twilight gate opens (1 at the horizon); `rate` (3, 0..60)
//            specks per second at full strength; `box_xz_m` (60, 1..200) half extent of the spawn box around the camera;
//            `height_m` (45, 1..120) height of the box above the camera; `size_m` (0.35, 0.05..2); `lifetime_s` (14,
//            1..40); `speed` (9, 0..40) the flight speed in m/s; `heading_deg` (70, 0..360) the direction of travel on the
//            ground plane (0 is +z, 90 is +x); `color` (0.08 0.07 0.07, linear); `max_particles` (64, 8..1024); `seed`
//            (1); `strength` (1, 0..1) scales every effect, 0 = off exactly.
//
// The amount is max(manual, migration(phase)) * twilight_gate * (1 - rain) times strength. The migration is the larger of
// two trapezoids in the signed circular distance of the phase from a season centre (like pollen.h, so the year start
// wraps safely); the twilight gate is 1 - smoothstep(0, twilight_width, |sun_y|). No Season gives the manual floor. The
// look turns the amount into the emitter description the extract (26b) adds as a local alpha-blended (not additive)
// render::ParticleEmitter: a fast, straight stream on the ground plane. The amount uses polynomials only, in a fixed
// order, no rand. `enabled` 0, `strength` 0 and a missing component (a null pointer) give an amount of 0 and the inactive
// zero look as exact values, which the bit-identical render gates rely on.

#include <algorithm>
#include <cmath>

#include "mce/kit/season.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce::kit {

struct Flock {
    f32 migration_full = 0.06f;
    f32 migration_end = 0.12f;
    f32 manual = 0.0f;
    f32 twilight_width = 0.25f;
    f32 rate = 3.0f;
    f32 box_xz_m = 60.0f;
    f32 height_m = 45.0f;
    f32 size_m = 0.35f;
    f32 lifetime_s = 14.0f;
    f32 speed = 9.0f;
    f32 heading_deg = 70.0f;
    f32 strength = 1.0f;
    math::vec3 color{0.08f, 0.07f, 0.07f};
    u32 max_particles = 64;
    u32 seed = 1;
    u8 enabled = 1;
    u8 pad_[3] = {};  // explicit pad (state_hash hashes whole columns); always zero
};
MCE_COMPONENT(Flock, (enabled, Save, Range{0, 1}, Help{"0: the flocks are off"}),
              (migration_full, Save, Range{0, 0.5f}, Help{"half-width in season phase of full strength around each migration centre"}),
              (migration_end, Save, Range{0, 0.5f}, Help{"half-width in season phase beyond which there are no flocks"}),
              (manual, Save, Range{0, 1}, Help{"floor for the migration; shows birds all year"}),
              (twilight_width, Save, Range{0.01f, 1}, Help{"sun height |y| below which the twilight gate opens"}),
              (rate, Save, Range{0, 60}, Help{"specks per second at full strength"}),
              (box_xz_m, Save, Range{1, 200}, Help{"half extent of the spawn box around the camera, metres"}),
              (height_m, Save, Range{1, 120}, Help{"height of the spawn box above the camera, metres"}),
              (size_m, Save, Range{0.05f, 2}, Help{"bird sprite size, metres"}),
              (lifetime_s, Save, Range{1, 40}, Help{"bird lifetime, seconds"}),
              (speed, Save, Range{0, 40}, Help{"flight speed, metres per second"}),
              (heading_deg, Save, Range{0, 360}, Help{"direction of travel on the ground plane, degrees (0 is +z, 90 is +x)"}),
              (color, Save, Help{"bird colour, linear"}),
              (max_particles, Save, Range{8, 1024}, Help{"particle budget of the flock emitter"}),
              (seed, Save, Help{"random seed of the flock emitter"}),
              (strength, Save, Range{0, 1}, Help{"scales every effect; 0 is off exactly"}))

// The emitter description for an amount. At amount 0 (and for a disabled or zero-strength component) it is `active =
// false` and every other field zero. The extract copies the fields into a render::ParticleEmitter: `direction` is a unit
// vector on the ground plane (y = 0), no gravity, almost no drag, `color_start` fades out to `color_end` (same rgb,
// alpha 0).
struct FlockLook {
    bool active = false;
    f32 rate = 0.0f;
    f32 speed_min = 0.0f;
    f32 speed_max = 0.0f;
    math::vec3 direction{0.0f, 0.0f, 0.0f};
    f32 spread = 0.0f;
    math::vec3 gravity{0.0f, 0.0f, 0.0f};
    f32 drag = 0.0f;
    f32 size = 0.0f;
    f32 lifetime_min = 0.0f;
    f32 lifetime_max = 0.0f;
    math::vec4 color_start{0.0f, 0.0f, 0.0f, 0.0f};
    math::vec4 color_end{0.0f, 0.0f, 0.0f, 0.0f};
    f32 box = 0.0f;
    f32 height = 0.0f;
};

namespace detail {

inline f32 flock_unit(f32 v, f32 def) { return season_clamp(v, def, 0.0f, 1.0f); }

// Almost no drag: a bird keeps its speed across the box.
inline constexpr f32 kFlockDrag = 0.01f;
inline constexpr f32 kFlockPi = 3.14159265358979f;

// The unsigned circular distance of a year phase (0..1) from a season centre, in [0, 0.5]. The signed distance is
// d - floor(d + 0.5) as in pollen.h; the absolute value is the trapezoid argument.
inline f32 flock_dist(f32 phase, f32 centre) {
    const f32 d = phase - centre;
    return std::fabs(d - std::floor(d + 0.5f));
}

// One migration trapezoid: 1 inside `full`, 0 beyond `end`, a smoothstep between.
inline f32 flock_trapezoid(f32 dist, f32 full, f32 end) { return 1.0f - season_smooth(full, end, dist); }

}  // namespace detail

// The flock amount 0..1. `phase` is the year phase, `sun_y` the y of the unit sun direction (-1..1, 0 at the horizon;
// NaN counts as high noon, so the gate is closed), `rain` the 0..1 rain intensity. A null or disabled Season has no
// migration, so the amount is the manual floor (still gated by the twilight and the rain). Exactly 0 for `enabled` 0 and
// `strength` 0.
inline f32 flock_amount(const Flock& f, const Season* season, f32 phase, f32 sun_y, f32 rain) {
    if (f.enabled == 0) return 0.0f;
    const f32 st = detail::flock_unit(f.strength, 1.0f);
    if (!(st > 0.0f)) return 0.0f;
    f32 full = detail::season_clamp(f.migration_full, 0.06f, 0.0f, 0.5f);
    f32 end = detail::season_clamp(f.migration_end, 0.12f, 0.0f, 0.5f);
    if (!(full < end)) {
        full = 0.06f;
        end = 0.12f;
    }
    f32 migration = 0.0f;
    if (season != nullptr && season->enabled != 0) {
        const f32 p = detail::season_wrap(phase);
        const f32 spring = detail::flock_trapezoid(detail::flock_dist(p, kSeasonSpringCentre), full, end);
        const f32 autumn = detail::flock_trapezoid(detail::flock_dist(p, kSeasonAutumnCentre), full, end);
        migration = std::max(spring, autumn);
    }
    const f32 manual = detail::flock_unit(f.manual, 0.0f);
    const f32 base = std::max(manual, migration);
    const f32 width = detail::season_clamp(f.twilight_width, 0.25f, 0.01f, 1.0f);
    const f32 h = std::isnan(sun_y) ? 1.0f : std::fabs(sun_y);
    const f32 gate = 1.0f - detail::season_smooth(0.0f, width, h);
    const f32 dry = 1.0f - detail::flock_unit(rain, 0.0f);
    return std::clamp(base * gate * dry * st, 0.0f, 1.0f);
}

// The emitter look at an amount (clamped to 0..1, NaN is 0).
inline FlockLook flock_look(const Flock& f, f32 amount) {
    FlockLook out;
    if (f.enabled == 0 || !(detail::flock_unit(f.strength, 1.0f) > 0.0f)) return out;
    const f32 a = detail::flock_unit(amount, 0.0f);
    if (!(a > 0.0f)) return out;
    out.active = true;
    out.rate = detail::season_clamp(f.rate, 3.0f, 0.0f, 60.0f) * a;
    const f32 sp = detail::season_clamp(f.speed, 9.0f, 0.0f, 40.0f);
    out.speed_min = 0.9f * sp;
    out.speed_max = sp;
    const f32 deg = detail::season_clamp(f.heading_deg, 70.0f, 0.0f, 360.0f);
    const f32 rad = deg * (detail::kFlockPi / 180.0f);
    out.direction = math::vec3{std::sin(rad), 0.0f, std::cos(rad)};
    out.spread = 0.03f;
    out.gravity = math::vec3{0.0f, 0.0f, 0.0f};
    out.drag = detail::kFlockDrag;
    out.size = detail::season_clamp(f.size_m, 0.35f, 0.05f, 2.0f);
    const f32 life = detail::season_clamp(f.lifetime_s, 14.0f, 1.0f, 40.0f);
    out.lifetime_min = 0.8f * life;
    out.lifetime_max = life;
    const math::vec3 rgb{detail::season_clamp(f.color.x, 0.08f, 0.0f, 1.0f), detail::season_clamp(f.color.y, 0.07f, 0.0f, 1.0f),
                         detail::season_clamp(f.color.z, 0.07f, 0.0f, 1.0f)};
    out.color_start = math::vec4{rgb.x, rgb.y, rgb.z, 0.85f * a};
    out.color_end = math::vec4{rgb.x, rgb.y, rgb.z, 0.0f};
    out.box = detail::season_clamp(f.box_xz_m, 60.0f, 1.0f, 200.0f);
    out.height = detail::season_clamp(f.height_m, 45.0f, 1.0f, 120.0f);
    return out;
}

// The same helpers for a component that may be missing: a null pointer is amount 0 and the inactive look.
inline f32 flock_amount(const Flock* f, const Season* season, f32 phase, f32 sun_y, f32 rain) {
    return f ? flock_amount(*f, season, phase, sun_y, rain) : 0.0f;
}
inline FlockLook flock_look(const Flock* f, f32 amount) { return f ? flock_look(*f, amount) : FlockLook{}; }

}  // namespace mce::kit
