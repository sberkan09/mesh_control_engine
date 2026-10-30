// Rainbow (M-RAINBOW 17a): the level is 0 by night, with the sun too high, while it rains, on a dry ground and under a
// thick cloud, and rises after the rain stops; the intensity peaks near 42 and 51 degrees, is 0 far away, the secondary
// scales with `secondary`; the primary is red outside and violet inside, the secondary reversed, all within [0, 1]; NaN,
// inverted and zero ranges fall back; the helpers agree in one run and in a split run around a save; the component
// round-trips save/load; the Weather, WorldClock, Moon, Aurora and Water data are the same with and without a Rainbow.

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <vector>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/world.h"
#include "mce/kit/aurora.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/moon.h"
#include "mce/kit/rainbow.h"
#include "mce/kit/water.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();
constexpr f32 kInf = std::numeric_limits<f32>::infinity();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

bool in01(math::vec3 c) {
    return c.x >= 0.0f && c.x <= 1.0f && c.y >= 0.0f && c.y <= 1.0f && c.z >= 0.0f && c.z <= 1.0f;
}

// A clear afternoon after rain: sun 25 degrees, wet ground, no rain, no cloud.
f32 clear_level(const Rainbow& r) { return rainbow_level(r, 25.0f, 0.8f, 0.0f, 0.0f); }

}  // namespace

TEST_CASE("rainbow: defaults") {
    const Rainbow r;
    CHECK(r.enabled == 1);
    CHECK(r.strength == 1.0f);
    CHECK(r.band_width == 1.0f);
    CHECK(r.secondary == 1.0f);
    CHECK(r.min_wetness == 0.25f);
    CHECK(r.max_sun_elevation_deg == 42.0f);
    CHECK(r.pad_[0] == 0);
    CHECK(r.pad_[1] == 0);
    CHECK(r.pad_[2] == 0);
}

TEST_CASE("rainbow: the level is 0 by night, with the sun too high, while raining, on dry ground and under thick cloud") {
    const Rainbow r;
    CHECK(clear_level(r) > 0.9f);
    // night: the sun at or below the horizon
    for (f32 el : {-90.0f, -30.0f, -0.1f, 0.0f}) CHECK(rainbow_level(r, el, 0.8f, 0.0f, 0.0f) == 0.0f);
    // the sun too high
    for (f32 el : {42.0f, 50.0f, 60.0f, 90.0f}) CHECK(rainbow_level(r, el, 0.8f, 0.0f, 0.0f) == 0.0f);
    // raining
    CHECK(rainbow_level(r, 25.0f, 0.8f, 1.0f, 0.0f) == 0.0f);
    CHECK(rainbow_level(r, 25.0f, 0.8f, 0.5f, 0.0f) < 0.6f * clear_level(r));
    // dry ground
    CHECK(rainbow_level(r, 25.0f, 0.0f, 0.0f, 0.0f) == 0.0f);
    CHECK(rainbow_level(r, 25.0f, 0.1f, 0.0f, 0.0f) == 0.0f);  // below half the threshold
    // thick cloud
    CHECK(rainbow_level(r, 25.0f, 0.8f, 0.0f, 1.0f) == 0.0f);
    CHECK(rainbow_level(r, 25.0f, 0.8f, 0.0f, 0.9f) < 0.02f);
    // off and zero strength are exactly 0
    Rainbow off;
    off.enabled = 0;
    CHECK(clear_level(off) == 0.0f);
    Rainbow zero;
    zero.strength = 0.0f;
    CHECK(clear_level(zero) == 0.0f);
    CHECK(rainbow_intensity(off, 42.0f) == 0.0f);
    CHECK(rainbow_intensity(zero, 42.0f) == 0.0f);
}

TEST_CASE("rainbow: the sun gate rises to 3 degrees and fades out toward the maximum elevation") {
    const Rainbow r;
    CHECK(same_bits(rainbow_level(r, 3.0f, 0.8f, 0.0f, 0.0f), rainbow_level(r, 20.0f, 0.8f, 0.0f, 0.0f)));
    f32 prev = 0.0f;
    for (f32 el = 0.0f; el <= 3.0f; el += 0.1f) {
        const f32 v = rainbow_level(r, el, 0.8f, 0.0f, 0.0f);
        CHECK(v >= prev);
        prev = v;
    }
    prev = rainbow_level(r, 32.0f, 0.8f, 0.0f, 0.0f);
    for (f32 el = 32.0f; el <= 42.0f; el += 0.25f) {
        const f32 v = rainbow_level(r, el, 0.8f, 0.0f, 0.0f);
        CHECK(v <= prev);
        prev = v;
    }
    Rainbow low;
    low.max_sun_elevation_deg = 20.0f;
    CHECK(rainbow_level(low, 25.0f, 0.8f, 0.0f, 0.0f) == 0.0f);
    CHECK(rainbow_level(low, 8.0f, 0.8f, 0.0f, 0.0f) > 0.9f);
}

TEST_CASE("rainbow: the level rises after the rain stops and falls as the ground dries") {
    const Rainbow r;
    // rain stops: the intensity falls to 0 while the ground is still wet
    f32 prev = -1.0f;
    for (f32 rain = 1.0f; rain >= 0.0f; rain -= 0.125f) {
        const f32 v = rainbow_level(r, 25.0f, 0.9f, rain, 0.3f);
        CHECK(v >= prev);
        prev = v;
    }
    CHECK(prev > 0.4f);
    // the ground dries: wetness 0.9 -> 0
    prev = 1e9f;
    for (f32 wet = 0.9f; wet >= 0.0f; wet -= 0.05f) {
        const f32 v = rainbow_level(r, 25.0f, wet, 0.0f, 0.0f);
        CHECK(v <= prev);
        prev = v;
    }
    CHECK(prev == 0.0f);
    // strength is the gain
    Rainbow s;
    s.strength = 2.0f;
    CHECK(clear_level(s) == doctest::Approx(2.0f * clear_level(r)));
    Rainbow wetter;
    wetter.min_wetness = 0.9f;
    CHECK(rainbow_level(wetter, 25.0f, 0.4f, 0.0f, 0.0f) == 0.0f);
}

TEST_CASE("rainbow: the intensity peaks near 42 and 51 degrees and is 0 far away") {
    const Rainbow r;
    CHECK(rainbow_intensity(r, 42.0f) == 1.0f);
    CHECK(rainbow_intensity(r, 51.0f) == doctest::Approx(0.35f));
    for (f32 a = 30.0f; a <= 60.0f; a += 0.05f) {
        const f32 v = rainbow_intensity(r, a);
        CHECK(v >= 0.0f);
        CHECK(v <= 1.0f);
        if (a < 41.0f || (a > 43.0f && a < 49.0f)) CHECK(v < 1.0f);
        CHECK(v <= rainbow_intensity(r, 42.0f));
    }
    for (f32 a : {-10.0f, 0.0f, 20.0f, 38.0f, 45.0f, 46.5f, 47.0f, 55.0f, 90.0f, 180.0f}) CHECK(rainbow_intensity(r, a) == 0.0f);
    // the primary is the brightest, the secondary only a local peak
    CHECK(rainbow_intensity(r, 51.0f) > rainbow_intensity(r, 49.5f));
    CHECK(rainbow_intensity(r, 51.0f) > rainbow_intensity(r, 52.5f));
    // the Alexander band between the bows is dark
    CHECK(rainbow_intensity(r, 46.0f) == 0.0f);
    // band_width scales the widths
    Rainbow wide, thin;
    wide.band_width = 2.0f;
    thin.band_width = 0.5f;
    CHECK(rainbow_intensity(wide, 43.0f) > rainbow_intensity(r, 43.0f));
    CHECK(rainbow_intensity(thin, 43.0f) == 0.0f);
    CHECK(rainbow_intensity(wide, 42.0f) == 1.0f);
}

TEST_CASE("rainbow: the secondary scales with `secondary` and is 0 for secondary = 0") {
    Rainbow r;
    const f32 full = rainbow_intensity(r, 51.0f);
    r.secondary = 0.5f;
    CHECK(rainbow_intensity(r, 51.0f) == doctest::Approx(0.5f * full));
    r.secondary = 0.0f;
    CHECK(rainbow_intensity(r, 51.0f) == 0.0f);
    CHECK(rainbow_intensity(r, 52.0f) == 0.0f);
    CHECK(rainbow_intensity(r, 42.0f) == 1.0f);  // the primary is unchanged
}

TEST_CASE("rainbow: the primary is red outside and violet inside, the secondary reversed, within [0, 1]") {
    const math::vec3 outer = rainbow_color(43.5f);
    const math::vec3 inner = rainbow_color(40.5f);
    CHECK(outer.x > outer.z);
    CHECK(inner.z > inner.x);
    CHECK(outer.x > inner.x);
    CHECK(inner.z > outer.z);
    // reversed for the secondary: violet outside, red inside
    const math::vec3 s_outer = rainbow_color(53.0f);
    const math::vec3 s_inner = rainbow_color(49.0f);
    CHECK(s_outer.z > s_outer.x);
    CHECK(s_inner.x > s_inner.z);
    // the ends of the ramp
    CHECK(rainbow_color(30.0f).z > rainbow_color(30.0f).x);
    CHECK(rainbow_color(44.0f).x == 1.0f);
    CHECK(rainbow_color(0.0f).z > 0.9f);
    CHECK(rainbow_color(90.0f).z > 0.9f);
    for (f32 a = -10.0f; a <= 100.0f; a += 0.05f) CHECK(in01(rainbow_color(a)));
    CHECK(in01(rainbow_color(kNaN)));
    CHECK(in01(rainbow_color(kInf)));
    CHECK(in01(rainbow_color(-kInf)));
    // deterministic
    CHECK(same_bits(rainbow_color(42.37f).y, rainbow_color(42.37f).y));
}

TEST_CASE("rainbow: NaN, inverted and out-of-range authoring falls back or clamps") {
    const Rainbow def;
    Rainbow n;
    n.strength = kNaN;
    n.band_width = kNaN;
    n.secondary = kNaN;
    n.min_wetness = kNaN;
    n.max_sun_elevation_deg = kNaN;
    for (f32 el : {-5.0f, 1.0f, 10.0f, 25.0f, 38.0f, 60.0f}) {
        CHECK(same_bits(rainbow_level(n, el, 0.7f, 0.1f, 0.2f), rainbow_level(def, el, 0.7f, 0.1f, 0.2f)));
    }
    for (f32 a = 38.0f; a <= 56.0f; a += 0.25f) CHECK(same_bits(rainbow_intensity(n, a), rainbow_intensity(def, a)));
    // a maximum below 10 clamps to 10 (the sun gate stays a proper rise then fade); a too-high one clamps to 60
    Rainbow inv, ten;
    inv.max_sun_elevation_deg = 3.0f;
    ten.max_sun_elevation_deg = 10.0f;
    for (f32 el : {1.0f, 5.0f, 9.0f, 25.0f}) CHECK(same_bits(rainbow_level(inv, el, 0.8f, 0.0f, 0.0f), rainbow_level(ten, el, 0.8f, 0.0f, 0.0f)));
    Rainbow zw;
    zw.band_width = 0.0f;
    CHECK(same_bits(rainbow_intensity(zw, 43.0f), rainbow_intensity(def, 43.0f)));
    Rainbow hi;
    hi.max_sun_elevation_deg = 500.0f;
    CHECK(rainbow_level(hi, 55.0f, 0.8f, 0.0f, 0.0f) > 0.0f);
    // NaN and infinite inputs never produce NaN
    for (f32 v : {kNaN, kInf, -kInf}) {
        CHECK(std::isfinite(rainbow_level(def, v, 0.8f, 0.0f, 0.0f)));
        CHECK(std::isfinite(rainbow_level(def, 25.0f, v, v, v)));
        CHECK(std::isfinite(rainbow_intensity(def, v)));
    }
    Rainbow inf;
    inf.strength = kInf;
    inf.band_width = kInf;
    inf.secondary = -kInf;
    CHECK(std::isfinite(clear_level(inf)));
    CHECK(clear_level(inf) <= 4.0f);
    CHECK(std::isfinite(rainbow_intensity(inf, 42.0f)));
}

namespace {

struct Rig {
    World w;
    u64 t = 0;
    Entity wxe, re, ce, me, ae, we;
    explicit Rig(bool with_rainbow, f32 hour = 15.0f) {
        w.register_components<SaveKey>();
        install_weather(w, nullptr);
        if (Physics3D* p = w.try_resource<Physics3D>()) p->auto_step = 0;
        Weather c;
        c.target = WeatherKind::Rain;
        c.transition_seconds = 0.5f;
        wxe = w.spawn_with(SaveKey{1}, c);
        if (with_rainbow) {
            Rainbow r;
            r.enabled = 0;
            re = w.spawn_with(SaveKey{2}, r);
        }
        WorldClock k;
        k.hour = hour;
        k.day_length_seconds = 20.0f;
        ce = w.spawn_with(SaveKey{3}, k);
        me = w.spawn_with(SaveKey{4}, Moon{});
        ae = w.spawn_with(SaveKey{5}, Aurora{});
        we = w.spawn_with(SaveKey{6}, Water{});
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    // The helpers fed from the live weather state, packed into bits for an exact comparison.
    std::vector<u32> sample(const Rainbow& r) {
        const Weather* wx = w.try_get<Weather>(wxe);
        const WorldClock* k = w.try_get<WorldClock>(ce);
        const f32 el = 90.0f - std::fabs(k->hour - 12.0f) * 15.0f;
        std::vector<u32> bits;
        bits.push_back(std::bit_cast<u32>(rainbow_level(r, el, std::max(wx->wetness, 0.8f), 0.0f, 0.25f)));
        for (const f32 a : {40.5f, 42.0f, 43.5f, 49.0f, 51.0f, 53.0f}) {
            const math::vec3 c = rainbow_color(a);
            bits.push_back(std::bit_cast<u32>(rainbow_intensity(r, a)));
            bits.push_back(std::bit_cast<u32>(c.x));
            bits.push_back(std::bit_cast<u32>(c.y));
            bits.push_back(std::bit_cast<u32>(c.z));
        }
        return bits;
    }
};

}  // namespace

TEST_CASE("rainbow: the component round-trips save and load") {
    Array<byte> bytes;
    u64 hash_a = 0;
    {
        Rig a(true);
        Rainbow& m = *a.w.try_get_mut<Rainbow>(a.re);
        m.enabled = 1;
        m.strength = 2.5f;
        m.band_width = 1.5f;
        m.secondary = 0.5f;
        m.min_wetness = 0.4f;
        m.max_sun_elevation_deg = 35.0f;
        a.run(30);
        save::write_save(a.w, {}, bytes);
        hash_a = a.w.state_hash();
    }
    Rig b(true);
    b.run(5);
    REQUIRE(save::read_save(b.w, bytes.span()));
    const Rainbow& m = *b.w.try_get<Rainbow>(b.re);
    CHECK(m.enabled == 1);
    CHECK(m.strength == 2.5f);
    CHECK(m.band_width == 1.5f);
    CHECK(m.secondary == 0.5f);
    CHECK(m.min_wetness == 0.4f);
    CHECK(m.max_sun_elevation_deg == 35.0f);
    CHECK(m.pad_[0] == 0);
    CHECK(m.pad_[1] == 0);
    CHECK(m.pad_[2] == 0);
    CHECK(b.w.state_hash() == hash_a);
}

TEST_CASE("rainbow: the Weather, WorldClock, Moon, Aurora and Water data are the same with and without a Rainbow") {
    Rig a(false), b(true);
    for (u32 i = 0; i < 300; ++i) {
        a.run(1);
        b.run(1);
        const Weather& wa = *a.w.try_get<Weather>(a.wxe);
        const Weather& wb = *b.w.try_get<Weather>(b.wxe);
        CHECK(wa.current == wb.current);
        CHECK(same_bits(wa.intensity, wb.intensity));
        CHECK(same_bits(wa.wind, wb.wind));
        CHECK(same_bits(wa.wetness, wb.wetness));
        CHECK(same_bits(wa.flash_mul, wb.flash_mul));
        CHECK(wa.counter == wb.counter);
        CHECK(wa.timer == wb.timer);
        CHECK(wa.fade == wb.fade);
        CHECK(wa.fade_den == wb.fade_den);
        CHECK(wa.wet_q == wb.wet_q);
        CHECK(wa.env_valid == wb.env_valid);
        for (u32 k = 0; k < 15; ++k) {
            CHECK(same_bits(wa.env_base[k], wb.env_base[k]));
            CHECK(same_bits(wa.env_applied[k], wb.env_applied[k]));
        }
        const WorldClock& ka = *a.w.try_get<WorldClock>(a.ce);
        const WorldClock& kb = *b.w.try_get<WorldClock>(b.ce);
        CHECK(same_bits(ka.hour, kb.hour));
        CHECK(same_bits(ka.hour_seen, kb.hour_seen));
        CHECK(ka.day == kb.day);
        CHECK(ka.phase == kb.phase);
        CHECK(ka.phase_den == kb.phase_den);
        CHECK(ka.started == kb.started);
        const Moon& ma = *a.w.try_get<Moon>(a.me);
        const Moon& mb = *b.w.try_get<Moon>(b.me);
        CHECK(same_bits(ma.phase_offset, mb.phase_offset));
        CHECK(same_bits(ma.cycle_days, mb.cycle_days));
        CHECK(ma.enabled == mb.enabled);
        const Aurora& aa = *a.w.try_get<Aurora>(a.ae);
        const Aurora& ab = *b.w.try_get<Aurora>(b.ae);
        CHECK(same_bits(aa.latitude_deg, ab.latitude_deg));
        CHECK(same_bits(aa.strength, ab.strength));
        CHECK(aa.enabled == ab.enabled);
        const Water& wta = *a.w.try_get<Water>(a.we);
        const Water& wtb = *b.w.try_get<Water>(b.we);
        CHECK(same_bits(wta.level_y, wtb.level_y));
        CHECK(same_bits(wta.amplitude, wtb.amplitude));
        CHECK(wta.enabled == wtb.enabled);
    }
    // the rainbow code never writes the component either: it is still as authored
    const Rainbow& m = *b.w.try_get<Rainbow>(b.re);
    CHECK(m.strength == 1.0f);
    CHECK(m.enabled == 0);
    Rig c(false), d(false);
    c.run(300);
    d.run(300);
    CHECK(c.w.state_hash() == d.w.state_hash());
}

TEST_CASE("rainbow: the helpers at fixed ticks are the same in one go and in two halves") {
    constexpr u32 kN = 600;
    constexpr u32 kEvery = 20;
    const Rainbow authored;
    std::vector<std::vector<u32>> whole;
    {
        Rig a(true);
        for (u32 i = 0; i < kN; ++i) {
            a.run(1);
            if ((i + 1) % kEvery == 0) whole.push_back(a.sample(authored));
        }
    }
    CHECK(whole.size() == kN / kEvery);
    bool moves = false;
    for (usize i = 1; i < whole.size(); ++i) moves = moves || whole[i][0] != whole[i - 1][0];
    CHECK(moves);

    std::vector<std::vector<u32>> halves;
    Array<byte> bytes;
    {
        Rig a(true);
        for (u32 i = 0; i < kN / 2; ++i) {
            a.run(1);
            if ((i + 1) % kEvery == 0) halves.push_back(a.sample(authored));
        }
        save::write_save(a.w, {}, bytes);
    }
    Rig b(true);
    b.run(11);  // another state, replaced by the save
    REQUIRE(save::read_save(b.w, bytes.span()));
    b.t = kN / 2;
    for (u32 i = kN / 2; i < kN; ++i) {
        b.run(1);
        if ((i + 1) % kEvery == 0) halves.push_back(b.sample(authored));
    }
    REQUIRE(halves.size() == whole.size());
    for (usize i = 0; i < whole.size(); ++i) CHECK(whole[i] == halves[i]);
}
