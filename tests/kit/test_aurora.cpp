// Aurora (M-AURORA 16a): the latitude gate is 0 below gate_low, 1 above gate_high, monotone between and mirrored for a
// negative latitude; aurora_level is 0 by day with night_only, with a closed gate, a zero strength or enabled 0, and falls
// with cloud coverage; the curtain height keeps lower < upper over the whole arc and time; the intensity stays in [0, 1],
// is zero outside the arc and repeats after the period; the band colour is low_color at the base and high_color at the
// top and stays within [0, 1]; NaN, inverted and zero ranges fall back or clamp; the helpers agree in one run and in a
// split run around a save; the component round-trips save/load; the Weather, WorldClock, Moon and Water data are the same
// with and without an Aurora.

#include <doctest/doctest.h>

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
bool same_vec(math::vec3 a, math::vec3 b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }

constexpr f32 kTimes[] = {0.0f, 0.5f, 3.25f, 17.0f, 40.75f, 63.5f};

}  // namespace

TEST_CASE("aurora: defaults") {
    const Aurora a;
    CHECK(a.enabled == 1);
    CHECK(a.latitude_deg == 68.0f);
    CHECK(a.gate_low_deg == 55.0f);
    CHECK(a.gate_high_deg == 65.0f);
    CHECK(a.strength == 1.0f);
    CHECK(a.curtain_count == 3);
    CHECK(a.base_height == 0.18f);
    CHECK(a.height_span == 0.35f);
    CHECK(a.azimuth_deg == 0.0f);
    CHECK(a.width_deg == 70.0f);
    CHECK(a.speed == 1.0f);
    CHECK(a.low_color.x == 0.10f);
    CHECK(a.low_color.y == 1.00f);
    CHECK(a.low_color.z == 0.35f);
    CHECK(a.high_color.x == 0.55f);
    CHECK(a.high_color.y == 0.15f);
    CHECK(a.high_color.z == 0.90f);
    CHECK(a.night_only == 1);
    CHECK(a.pad_[0] == 0);
}

TEST_CASE("aurora: the latitude gate is 0 below the low edge, 1 above the high edge and monotone between") {
    Aurora a;
    for (f32 lat : {0.0f, 20.0f, 54.9f, 55.0f}) {
        a.latitude_deg = lat;
        CHECK(aurora_latitude_gate(a) == 0.0f);
    }
    for (f32 lat : {65.0f, 65.1f, 80.0f, 90.0f}) {
        a.latitude_deg = lat;
        CHECK(aurora_latitude_gate(a) == 1.0f);
    }
    f32 prev = 0.0f;
    for (f32 lat = 55.0f; lat <= 65.0f; lat += 0.25f) {
        a.latitude_deg = lat;
        const f32 g = aurora_latitude_gate(a);
        CHECK(g >= prev);
        CHECK(g >= 0.0f);
        CHECK(g <= 1.0f);
        prev = g;
    }
    a.latitude_deg = 60.0f;
    CHECK(aurora_latitude_gate(a) == doctest::Approx(0.5f));
    for (f32 lat = 0.0f; lat <= 90.0f; lat += 1.5f) {  // mirrored: the gate reads the absolute value
        a.latitude_deg = lat;
        const f32 north = aurora_latitude_gate(a);
        a.latitude_deg = -lat;
        CHECK(same_bits(aurora_latitude_gate(a), north));
    }
    a.latitude_deg = 200.0f;  // beyond the Range clamps
    CHECK(aurora_latitude_gate(a) == 1.0f);
    a.latitude_deg = kNaN;  // a NaN latitude is closed
    CHECK(aurora_latitude_gate(a) == 0.0f);
}

TEST_CASE("aurora: NaN, inverted and zero gate ranges fall back to the defaults") {
    Aurora d;
    d.latitude_deg = 58.0f;
    const f32 want = aurora_latitude_gate(d);
    CHECK(want > 0.0f);
    CHECK(want < 1.0f);
    Aurora inv = d;
    inv.gate_low_deg = 70.0f;
    inv.gate_high_deg = 30.0f;
    CHECK(same_bits(aurora_latitude_gate(inv), want));
    Aurora zero = d;
    zero.gate_low_deg = 40.0f;
    zero.gate_high_deg = 40.0f;
    CHECK(same_bits(aurora_latitude_gate(zero), want));
    Aurora nan = d;
    nan.gate_low_deg = kNaN;
    nan.gate_high_deg = kNaN;
    CHECK(same_bits(aurora_latitude_gate(nan), want));
    Aurora oor = d;  // out of the Range clamps to 0..90, here an inverted pair after clamping
    oor.gate_low_deg = 500.0f;
    oor.gate_high_deg = -500.0f;
    CHECK(same_bits(aurora_latitude_gate(oor), want));
}

TEST_CASE("aurora: the level is 0 when off, by day with night_only, with a closed gate or a zero strength") {
    Aurora a;
    CHECK(aurora_level(a, 1.0f, 0.0f) == 1.0f);
    CHECK(aurora_level(a, 0.0f, 0.0f) == 0.0f);  // day with night_only
    CHECK(aurora_level(a, kNaN, 0.0f) == 0.0f);
    Aurora off = a;
    off.enabled = 0;
    CHECK(aurora_level(off, 1.0f, 0.0f) == 0.0f);
    Aurora closed = a;
    closed.latitude_deg = 30.0f;
    CHECK(aurora_level(closed, 1.0f, 0.0f) == 0.0f);
    Aurora none = a;
    none.strength = 0.0f;
    CHECK(aurora_level(none, 1.0f, 0.0f) == 0.0f);
    Aurora forced = a;  // night_only 0 forces it by day
    forced.night_only = 0;
    CHECK(aurora_level(forced, 0.0f, 0.0f) == 1.0f);
    Aurora strong = a;
    strong.strength = 99.0f;  // clamps to 4
    CHECK(aurora_level(strong, 1.0f, 0.0f) == 4.0f);
    Aurora nan = a;
    nan.strength = kNaN;  // the default 1
    CHECK(aurora_level(nan, 1.0f, 0.0f) == 1.0f);
    // half the night fade, half the level
    CHECK(aurora_level(a, 0.5f, 0.0f) == doctest::Approx(0.5f));
    // the level falls with the cloud coverage, to 15 percent when overcast
    f32 prev = 2.0f;
    for (f32 cov = 0.0f; cov <= 1.0f; cov += 0.125f) {
        const f32 l = aurora_level(a, 1.0f, cov);
        CHECK(l < prev);
        prev = l;
    }
    CHECK(aurora_level(a, 1.0f, 1.0f) == doctest::Approx(0.15f));
    CHECK(aurora_level(a, 1.0f, 7.0f) == aurora_level(a, 1.0f, 1.0f));
    CHECK(aurora_level(a, 1.0f, kNaN) == 1.0f);
}

TEST_CASE("aurora: the wrapped time stays in the period") {
    CHECK(aurora_wrap_time(0.0) == 0.0f);
    CHECK(aurora_wrap_time(64.0) == 0.0f);
    CHECK(aurora_wrap_time(-1.0) == 63.0f);
    CHECK(aurora_wrap_time(1.0e7 + 0.25) == doctest::Approx(static_cast<f32>(std::fmod(1.0e7 + 0.25, 64.0))));
    CHECK(aurora_wrap_time(std::numeric_limits<f64>::infinity()) == 0.0f);
    CHECK(aurora_wrap_time(std::numeric_limits<f64>::quiet_NaN()) == 0.0f);
    CHECK(kAuroraPeriodSeconds == 64.0);
}

TEST_CASE("aurora: the curtain edges keep lower < upper over the whole arc and time") {
    for (f32 base : {0.02f, 0.18f, 0.6f, 5.0f})
        for (f32 span : {0.05f, 0.35f, 0.8f, 5.0f})
            for (f32 speed : {0.0f, 1.0f, 2.5f, 4.0f}) {
                Aurora a;
                a.base_height = base;
                a.height_span = span;
                a.speed = speed;
                for (f32 u = 0.0f; u <= 1.0f; u += 0.05f)
                    for (f32 t = 0.0f; t < 64.0f; t += 1.7f) {
                        const math::vec2 h = aurora_curtain_height(a, u, t);
                        CHECK(h.x > 0.0f);
                        CHECK(h.x < h.y);
                        CHECK(h.y <= 1.0f);
                    }
            }
    // it moves with the time and with the arc, and is pure
    const Aurora a;
    const math::vec2 p = aurora_curtain_height(a, 0.3f, 1.0f), q = aurora_curtain_height(a, 0.3f, 5.0f);
    CHECK((p.x != q.x || p.y != q.y));
    const math::vec2 r = aurora_curtain_height(a, 0.7f, 1.0f);
    CHECK((p.x != r.x || p.y != r.y));
    const math::vec2 again = aurora_curtain_height(a, 0.3f, 1.0f);
    CHECK(same_bits(p.x, again.x));
    CHECK(same_bits(p.y, again.y));
    // speed 0 holds still
    Aurora still;
    still.speed = 0.0f;
    const math::vec2 s0 = aurora_curtain_height(still, 0.4f, 0.0f), s1 = aurora_curtain_height(still, 0.4f, 21.5f);
    CHECK(same_bits(s0.x, s1.x));
    CHECK(same_bits(s0.y, s1.y));
    // NaN inputs fall back
    const math::vec2 n = aurora_curtain_height(a, kNaN, kNaN);
    const math::vec2 d = aurora_curtain_height(a, 0.5f, 0.0f);
    CHECK(same_bits(n.x, d.x));
    CHECK(same_bits(n.y, d.y));
    Aurora bad;
    bad.base_height = kNaN;
    bad.height_span = kNaN;
    bad.speed = kNaN;
    const math::vec2 b = aurora_curtain_height(bad, 0.2f, 3.0f), c = aurora_curtain_height(a, 0.2f, 3.0f);
    CHECK(same_bits(b.x, c.x));
    CHECK(same_bits(b.y, c.y));
}

TEST_CASE("aurora: the intensity is in [0, 1], zero outside the arc and when closed") {
    const Aurora a;
    f32 hi = 0.0f;
    for (u32 count : {1u, 3u, 6u})
        for (f32 width : {10.0f, 70.0f, 180.0f})
            for (f32 speed : {0.0f, 1.0f, 4.0f}) {
                Aurora v = a;
                v.curtain_count = static_cast<u8>(count);
                v.width_deg = width;
                v.speed = speed;
                for (f32 u = 0.0f; u <= 1.0f; u += 0.03125f)
                    for (f32 h = 0.0f; h <= 1.0f; h += 0.05f)
                        for (f32 t : kTimes) {
                            const f32 i = aurora_curtain_intensity(v, u, h, t);
                            CHECK(i >= 0.0f);
                            CHECK(i <= 1.0f);
                            hi = std::max(hi, i);
                        }
            }
    CHECK(hi > 0.3f);  // and the curtains do shine
    for (f32 h : {0.0f, 0.3f, 1.0f})
        for (f32 t : kTimes) {
            CHECK(aurora_curtain_intensity(a, 0.0f, h, t) == 0.0f);
            CHECK(aurora_curtain_intensity(a, 1.0f, h, t) == 0.0f);
            CHECK(aurora_curtain_intensity(a, -0.5f, h, t) == 0.0f);
            CHECK(aurora_curtain_intensity(a, 1.5f, h, t) == 0.0f);
            CHECK(aurora_curtain_intensity(a, kNaN, h, t) == 0.0f);
        }
    for (f32 u : {0.25f, 0.5f, 0.75f}) {
        CHECK(aurora_curtain_intensity(a, u, 0.0f, 1.0f) == 0.0f);  // the sharp lower edge
        CHECK(aurora_curtain_intensity(a, u, -0.1f, 1.0f) == 0.0f);
        CHECK(aurora_curtain_intensity(a, u, 1.1f, 1.0f) == 0.0f);
        CHECK(aurora_curtain_intensity(a, u, kNaN, 1.0f) == 0.0f);
        CHECK(aurora_curtain_intensity(a, u, 0.3f, 1.0f) > 0.0f);
        // fades toward the top
        CHECK(aurora_curtain_intensity(a, u, 0.15f, 1.0f) > aurora_curtain_intensity(a, u, 0.95f, 1.0f));
    }
    Aurora off = a;
    off.enabled = 0;
    Aurora closed = a;
    closed.latitude_deg = 10.0f;
    Aurora none = a;
    none.strength = 0.0f;
    for (const Aurora* v : {&off, &closed, &none}) CHECK(aurora_curtain_intensity(*v, 0.5f, 0.3f, 1.0f) == 0.0f);
    // the arc ends fade: the middle is brighter than the edge on average
    f32 mid = 0.0f, edge = 0.0f;
    for (f32 t : kTimes)
        for (f32 h = 0.1f; h < 0.5f; h += 0.1f) {
            mid += aurora_curtain_intensity(a, 0.5f, h, t);
            edge += aurora_curtain_intensity(a, 0.03f, h, t);
        }
    CHECK(mid > edge);
}

TEST_CASE("aurora: the height, intensity and colour at t equal those at t plus the period") {
    const Aurora a;
    const f32 period = static_cast<f32>(kAuroraPeriodSeconds);
    for (f32 t : kTimes)
        for (f32 u = 0.05f; u < 1.0f; u += 0.1f) {
            const math::vec2 h0 = aurora_curtain_height(a, u, t), h1 = aurora_curtain_height(a, u, t + period);
            CHECK(same_bits(h0.x, h1.x));
            CHECK(same_bits(h0.y, h1.y));
            const math::vec2 h2 = aurora_curtain_height(a, u, t + 4.0f * period);
            CHECK(same_bits(h0.x, h2.x));
            for (f32 h = 0.1f; h <= 1.0f; h += 0.15f) {
                CHECK(same_bits(aurora_curtain_intensity(a, u, h, t), aurora_curtain_intensity(a, u, h, t + period)));
                CHECK(same_bits(aurora_curtain_intensity(a, u, h, t), aurora_curtain_intensity(a, u, h, t + 3.0f * period)));
            }
        }
    // it animates within the period, at every speed that moves
    for (f32 speed : {0.25f, 1.0f, 3.0f}) {
        Aurora v = a;
        v.speed = speed;
        CHECK(aurora_curtain_intensity(v, 0.4f, 0.3f, 1.0f) != aurora_curtain_intensity(v, 0.4f, 0.3f, 2.3f));
    }
    // a speed that is not a multiple of a quarter is quantised, so the period still divides every rate
    Aurora q = a;
    q.speed = 1.1f;
    CHECK(same_bits(aurora_curtain_intensity(q, 0.4f, 0.3f, 5.5f), aurora_curtain_intensity(q, 0.4f, 0.3f, 5.5f + period)));
}

TEST_CASE("aurora: the band colour is low_color at the base and high_color at the top, within [0, 1]") {
    const Aurora a;
    CHECK(same_vec(aurora_band_color(a, 0.0f), a.low_color));
    CHECK(same_vec(aurora_band_color(a, 0.55f), a.low_color));
    CHECK(same_vec(aurora_band_color(a, 1.0f), a.high_color));
    CHECK(same_vec(aurora_band_color(a, -3.0f), a.low_color));
    CHECK(same_vec(aurora_band_color(a, 9.0f), a.high_color));
    CHECK(same_vec(aurora_band_color(a, kNaN), a.low_color));
    // between the two, monotone from one to the other (the green falls, the red rises)
    f32 prev_g = 2.0f, prev_r = -1.0f;
    for (f32 h = 0.0f; h <= 1.0f; h += 0.05f) {
        const math::vec3 c = aurora_band_color(a, h);
        CHECK(c.y <= prev_g);
        CHECK(c.x >= prev_r);
        prev_g = c.y;
        prev_r = c.x;
        for (f32 v : {c.x, c.y, c.z}) {
            CHECK(v >= 0.0f);
            CHECK(v <= 1.0f);
        }
    }
    // the base is greener than the top; the top is more violet
    CHECK(aurora_band_color(a, 0.1f).y > aurora_band_color(a, 0.95f).y);
    CHECK(aurora_band_color(a, 0.95f).x > aurora_band_color(a, 0.1f).x);
    Aurora wild = a;  // authored colours outside 0..1 clamp, NaN channels mean the default
    wild.low_color = {-2.0f, 7.0f, kNaN};
    wild.high_color = {kInf, 0.5f, -kInf};
    const math::vec3 lo = aurora_band_color(wild, 0.0f), hi = aurora_band_color(wild, 1.0f);
    CHECK(same_vec(lo, math::vec3{0.0f, 1.0f, 0.35f}));
    CHECK(same_vec(hi, math::vec3{1.0f, 0.5f, 0.0f}));
}

TEST_CASE("aurora: NaN and infinite authoring never produce NaN") {
    Aurora n;
    n.latitude_deg = kNaN;
    n.strength = kNaN;
    n.base_height = kNaN;
    n.height_span = kNaN;
    n.azimuth_deg = kNaN;
    n.width_deg = kNaN;
    n.speed = kNaN;
    n.gate_low_deg = kNaN;
    n.gate_high_deg = kNaN;
    for (f32 u = 0.0f; u <= 1.0f; u += 0.1f)
        for (f32 t : kTimes) {
            const math::vec2 h = aurora_curtain_height(n, u, t);
            CHECK(std::isfinite(h.x));
            CHECK(std::isfinite(h.y));
            CHECK(std::isfinite(aurora_curtain_intensity(n, u, 0.4f, t)));
        }
    Aurora inf;
    inf.speed = kInf;
    inf.width_deg = kInf;
    inf.base_height = -kInf;
    for (f32 t : {0.0f, 7.0f, kInf, kNaN}) {
        CHECK(std::isfinite(aurora_curtain_intensity(inf, 0.5f, 0.4f, t)));
        CHECK(std::isfinite(aurora_curtain_height(inf, 0.5f, t).x));
    }
    CHECK(aurora_level(inf, kInf, kInf) >= 0.0f);
    CHECK(std::isfinite(aurora_level(inf, kInf, -kInf)));
}

namespace {

struct Rig {
    World w;
    u64 t = 0;
    Entity wxe, ae, ce, me, we;
    explicit Rig(bool with_aurora, f32 hour = 22.0f) {
        w.register_components<SaveKey>();
        install_weather(w, nullptr);
        if (Physics3D* p = w.try_resource<Physics3D>()) p->auto_step = 0;
        Weather c;
        c.target = WeatherKind::Rain;
        c.transition_seconds = 0.5f;
        wxe = w.spawn_with(SaveKey{1}, c);
        if (with_aurora) {
            Aurora a;
            a.enabled = 0;
            ae = w.spawn_with(SaveKey{2}, a);
        }
        WorldClock k;
        k.hour = hour;
        k.day_length_seconds = 20.0f;
        ce = w.spawn_with(SaveKey{3}, k);
        me = w.spawn_with(SaveKey{4}, Moon{});
        we = w.spawn_with(SaveKey{5}, Water{});
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    // The helpers at the current scene time, packed into bits for an exact comparison. The night fade is derived from
    // the clock hour the way a feed would: a smooth function of the hour.
    std::vector<u32> sample(const Aurora& a) {
        const f64 time = static_cast<f64>(t) * static_cast<f64>(kDt);
        const f32 tw = aurora_wrap_time(time);
        const WorldClock* k = w.try_get<WorldClock>(ce);
        const f32 fade = std::clamp(std::fabs(k->hour - 12.0f) / 12.0f, 0.0f, 1.0f);
        std::vector<u32> bits;
        bits.push_back(std::bit_cast<u32>(aurora_level(a, fade, 0.25f)));
        for (const f32 u : {0.2f, 0.5f, 0.8f}) {
            const math::vec2 h = aurora_curtain_height(a, u, tw);
            const math::vec3 c = aurora_band_color(a, u);
            bits.push_back(std::bit_cast<u32>(h.x));
            bits.push_back(std::bit_cast<u32>(h.y));
            bits.push_back(std::bit_cast<u32>(aurora_curtain_intensity(a, u, 0.3f, tw)));
            bits.push_back(std::bit_cast<u32>(c.x));
            bits.push_back(std::bit_cast<u32>(c.y));
            bits.push_back(std::bit_cast<u32>(c.z));
        }
        return bits;
    }
};

}  // namespace

TEST_CASE("aurora: the component round-trips save and load") {
    Array<byte> bytes;
    u64 hash_a = 0;
    {
        Rig a(true);
        Aurora& m = *a.w.try_get_mut<Aurora>(a.ae);
        m.enabled = 1;
        m.latitude_deg = -71.5f;
        m.gate_low_deg = 50.0f;
        m.gate_high_deg = 70.0f;
        m.strength = 2.5f;
        m.curtain_count = 5;
        m.base_height = 0.25f;
        m.height_span = 0.5f;
        m.azimuth_deg = 123.0f;
        m.width_deg = 99.0f;
        m.speed = 2.25f;
        m.low_color = {0.1f, 0.2f, 0.3f};
        m.high_color = {0.4f, 0.5f, 0.6f};
        m.night_only = 0;
        a.run(30);
        save::write_save(a.w, {}, bytes);
        hash_a = a.w.state_hash();
    }
    Rig b(true);
    b.run(5);
    REQUIRE(save::read_save(b.w, bytes.span()));
    const Aurora& m = *b.w.try_get<Aurora>(b.ae);
    CHECK(m.enabled == 1);
    CHECK(m.latitude_deg == -71.5f);
    CHECK(m.gate_low_deg == 50.0f);
    CHECK(m.gate_high_deg == 70.0f);
    CHECK(m.strength == 2.5f);
    CHECK(m.curtain_count == 5);
    CHECK(m.base_height == 0.25f);
    CHECK(m.height_span == 0.5f);
    CHECK(m.azimuth_deg == 123.0f);
    CHECK(m.width_deg == 99.0f);
    CHECK(m.speed == 2.25f);
    CHECK(m.low_color.x == 0.1f);
    CHECK(m.low_color.y == 0.2f);
    CHECK(m.low_color.z == 0.3f);
    CHECK(m.high_color.x == 0.4f);
    CHECK(m.high_color.y == 0.5f);
    CHECK(m.high_color.z == 0.6f);
    CHECK(m.night_only == 0);
    CHECK(m.pad_[0] == 0);
    CHECK(b.w.state_hash() == hash_a);
}

TEST_CASE("aurora: the Weather, WorldClock, Moon and Water data are the same with and without an Aurora") {
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
        const Water& wta = *a.w.try_get<Water>(a.we);
        const Water& wtb = *b.w.try_get<Water>(b.we);
        CHECK(same_bits(wta.level_y, wtb.level_y));
        CHECK(same_bits(wta.amplitude, wtb.amplitude));
        CHECK(wta.enabled == wtb.enabled);
    }
    // the aurora code never writes the component either: it is still as authored
    const Aurora& m = *b.w.try_get<Aurora>(b.ae);
    CHECK(m.strength == 1.0f);
    CHECK(m.enabled == 0);
    Rig c(false), d(false);
    c.run(300);
    d.run(300);
    CHECK(c.w.state_hash() == d.w.state_hash());
}

TEST_CASE("aurora: the helpers at fixed ticks are the same in one go and in two halves") {
    constexpr u32 kN = 4000;  // longer than one period (64 s = 3840 ticks)
    constexpr u32 kEvery = 100;
    const Aurora authored;
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
    for (usize i = 1; i < whole.size(); ++i) moves = moves || whole[i][3] != whole[i - 1][3];
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
