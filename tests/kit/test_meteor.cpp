// Meteor (M-METEOR 18a): the level is 0 by day with `night_only`, under thick cloud, in heavy rain, at `enabled` 0, at
// `rate` 0 and at `brightness` 0, and rises with the night fade between `min_darkness` and 1; the streak hash is a pure
// function of (slot, index, seed); start elevation, travel direction, start time and duration stay in their ranges and
// a streak ends inside its slot; the average number of active streaks per minute matches `rate` and never exceeds 3 in
// a slot; the head stays on the unit sphere and moves along a great circle; the envelope is 0 outside the streak and
// peaks inside; the pattern repeats after 60 s; NaN, inverted and zero ranges fall back; the helpers agree in one run
// and in a split run around a save; the component round-trips save/load; the Weather, WorldClock, Moon, Aurora and
// Rainbow data are the same with and without a Meteor.

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
#include "mce/kit/meteor.h"
#include "mce/kit/moon.h"
#include "mce/kit/rainbow.h"
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

bool same_streak(const MeteorStreak& a, const MeteorStreak& b) {
    return same_vec(a.start, b.start) && same_vec(a.travel, b.travel) && same_bits(a.start_time, b.start_time) &&
           same_bits(a.duration, b.duration) && same_bits(a.angular_speed, b.angular_speed) &&
           same_bits(a.tail_angle, b.tail_angle) && a.active == b.active;
}

f32 dot3(math::vec3 a, math::vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

math::vec3 cross3(math::vec3 a, math::vec3 b) {
    return math::vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// A dark clear night.
f32 night_level(const Meteor& m) { return meteor_level(m, 1.0f, 0.0f, 0.0f); }

}  // namespace

TEST_CASE("meteor: defaults") {
    const Meteor m;
    CHECK(m.enabled == 1);
    CHECK(m.rate == 4.0f);
    CHECK(m.brightness == 1.0f);
    CHECK(m.length == 1.0f);
    CHECK(m.speed == 1.0f);
    CHECK(m.min_darkness == 0.6f);
    CHECK(m.night_only == 1);
    CHECK(m.seed == 0);
    CHECK(m.pad_[0] == 0);
    CHECK(m.pad_[1] == 0);
}

TEST_CASE("meteor: the level is 0 by day, under thick cloud, in heavy rain and when off, rate 0 or brightness 0") {
    const Meteor m;
    CHECK(night_level(m) == 1.0f);
    CHECK(meteor_level(m, 0.0f, 0.0f, 0.0f) == 0.0f);  // day
    CHECK(meteor_level(m, 0.6f, 0.0f, 0.0f) == 0.0f);  // below min_darkness the gate is closed (exactly at it)
    CHECK(meteor_level(m, 1.0f, 1.0f, 0.0f) == 0.0f);  // thick cloud
    CHECK(meteor_level(m, 1.0f, 0.9f, 0.0f) < 0.02f);
    CHECK(meteor_level(m, 1.0f, 0.0f, 1.0f) == 0.0f);  // heavy rain
    CHECK(meteor_level(m, 1.0f, 0.0f, 0.5f) == doctest::Approx(0.5f));
    Meteor off;
    off.enabled = 0;
    CHECK(night_level(off) == 0.0f);
    Meteor norate;
    norate.rate = 0.0f;
    CHECK(night_level(norate) == 0.0f);
    Meteor nobright;
    nobright.brightness = 0.0f;
    CHECK(night_level(nobright) == 0.0f);
    // night_only 0 shows by day too
    Meteor forced;
    forced.night_only = 0;
    CHECK(meteor_level(forced, 0.0f, 0.0f, 0.0f) == 1.0f);
    CHECK(meteor_level(forced, 0.0f, 1.0f, 0.0f) == 0.0f);
    CHECK(meteor_level(forced, 0.0f, 0.0f, 1.0f) == 0.0f);
    // brightness and rate are not folded into the level
    Meteor bright;
    bright.brightness = 3.0f;
    bright.rate = 20.0f;
    CHECK(night_level(bright) == 1.0f);
    // off, rate 0 and brightness 0 make every streak inactive
    for (u32 slot = 0; slot < 12; ++slot)
        for (u32 k = 0; k < kMeteorMaxPerSlot; ++k) {
            CHECK(!meteor_make(off, slot, k).active);
            CHECK(!meteor_make(norate, slot, k).active);
            CHECK(!meteor_make(nobright, slot, k).active);
        }
}

TEST_CASE("meteor: the level rises with the night fade between min_darkness and 1") {
    const Meteor m;
    f32 prev = -1.0f;
    for (f32 f = 0.0f; f <= 1.0f; f += 0.02f) {
        const f32 v = meteor_level(m, f, 0.0f, 0.0f);
        CHECK(v >= prev);
        CHECK(v >= 0.0f);
        CHECK(v <= 1.0f);
        prev = v;
    }
    CHECK(meteor_level(m, 0.8f, 0.0f, 0.0f) == doctest::Approx(0.5f));  // the smoothstep midpoint of 0.6..1
    CHECK(meteor_level(m, 0.7f, 0.0f, 0.0f) > 0.0f);
    CHECK(meteor_level(m, 0.7f, 0.0f, 0.0f) < meteor_level(m, 0.9f, 0.0f, 0.0f));
    Meteor late;
    late.min_darkness = 0.9f;
    CHECK(meteor_level(late, 0.85f, 0.0f, 0.0f) == 0.0f);
    CHECK(meteor_level(late, 0.95f, 0.0f, 0.0f) > 0.0f);
    Meteor any;
    any.min_darkness = 0.0f;
    CHECK(meteor_level(any, 0.0f, 0.0f, 0.0f) == 0.0f);
    CHECK(meteor_level(any, 0.5f, 0.0f, 0.0f) > 0.0f);
}

TEST_CASE("meteor: a streak is a pure function of (slot, index, seed)") {
    const Meteor m;
    Meteor rate_big = m;
    rate_big.rate = 30.0f;
    for (u32 slot = 0; slot < 12; ++slot)
        for (u32 k = 0; k < kMeteorMaxPerSlot; ++k) {
            CHECK(same_streak(meteor_make(rate_big, slot, k), meteor_make(rate_big, slot, k)));
        }
    Meteor other = rate_big;
    other.seed = 7;
    u32 diff_seed = 0, diff_slot = 0, diff_index = 0;
    for (u32 slot = 0; slot < 12; ++slot) {
        for (u32 k = 0; k < kMeteorMaxPerSlot; ++k) {
            const MeteorStreak a = meteor_make(rate_big, slot, k);
            if (!same_vec(a.start, meteor_make(other, slot, k).start)) ++diff_seed;
            if (!same_vec(a.start, meteor_make(rate_big, slot + 12 * 7 + 1, k).start)) ++diff_slot;
            if (!same_vec(a.start, meteor_make(rate_big, slot, k + 1).start)) ++diff_index;
        }
    }
    CHECK(diff_seed == 36);
    CHECK(diff_slot == 36);
    CHECK(diff_index == 36);
    // the speed and length scale the motion, not the placement
    Meteor fast = rate_big;
    fast.speed = 2.0f;
    fast.length = 2.0f;
    const MeteorStreak a = meteor_make(rate_big, 3, 1);
    const MeteorStreak b = meteor_make(fast, 3, 1);
    CHECK(same_vec(a.start, b.start));
    CHECK(same_vec(a.travel, b.travel));
    CHECK(same_bits(a.start_time, b.start_time));
    CHECK(b.duration == doctest::Approx(a.duration * 0.5f));
    CHECK(b.angular_speed == doctest::Approx(a.angular_speed * 2.0f));
    CHECK(b.tail_angle == doctest::Approx(a.tail_angle * 2.0f));
}

TEST_CASE("meteor: start elevation, travel direction, start time and duration stay in range over 10000 slots") {
    Meteor m;
    m.rate = 30.0f;
    for (f32 speed : {0.5f, 1.0f, 2.0f}) {
        m.speed = speed;
        f32 min_y = 2.0f, max_y = -2.0f, min_dur = 1e9f, max_dur = 0.0f, max_end = 0.0f, max_start = 0.0f;
        for (u32 slot = 0; slot < 10000; ++slot) {
            for (u32 k = 0; k < kMeteorMaxPerSlot; ++k) {
                const MeteorStreak s = meteor_make(m, slot, k);
                CHECK(dot3(s.start, s.start) == doctest::Approx(1.0f).epsilon(1e-5));
                CHECK(dot3(s.travel, s.travel) == doctest::Approx(1.0f).epsilon(1e-5));
                CHECK(std::fabs(dot3(s.start, s.travel)) < 1e-5f);
                min_y = std::min(min_y, s.start.y);
                max_y = std::max(max_y, s.start.y);
                min_dur = std::min(min_dur, s.duration);
                max_dur = std::max(max_dur, s.duration);
                max_start = std::max(max_start, s.start_time);
                max_end = std::max(max_end, s.start_time + s.duration);
                // the arc is at most 45 degrees
                CHECK(s.angular_speed * s.duration <= 45.0f * 0.017453292f);
            }
        }
        CHECK(min_y >= 0.1f);
        CHECK(max_y <= 0.95f);
        CHECK(min_y < 0.12f);  // the range is used, not just a corner of it
        CHECK(max_y > 0.93f);
        CHECK(min_dur >= 0.4f / speed);
        CHECK(max_dur <= 1.2f / speed);
        CHECK(max_start <= 2.5f);
        CHECK(max_start > 2.4f);
        // a streak ends inside its own slot at every speed
        CHECK(max_end < kMeteorSlotSeconds);
    }
}

TEST_CASE("meteor: the average number of active streaks per minute is the rate and never exceeds 3 per slot") {
    for (f32 rate : {1.0f, 4.0f, 10.0f, 17.0f, 30.0f}) {
        Meteor m;
        m.rate = rate;
        m.seed = 11;
        constexpr u32 kMinutes = 2000;
        u64 total = 0;
        for (u32 slot = 0; slot < kMinutes * kMeteorSlotsPerPeriod; ++slot) {
            const u32 n = meteor_slot_count(m, slot);
            CHECK(n <= kMeteorMaxPerSlot);
            u32 active = 0;
            for (u32 k = 0; k < kMeteorMaxPerSlot; ++k) {
                if (meteor_make(m, slot, k).active) ++active;
            }
            CHECK(active == n);
            total += n;
        }
        const f64 per_minute = static_cast<f64>(total) / kMinutes;
        CHECK(per_minute == doctest::Approx(rate).epsilon(0.1));
    }
}

TEST_CASE("meteor: the head stays on the unit sphere and moves along a great circle") {
    Meteor m;
    m.rate = 30.0f;
    for (u32 slot = 0; slot < 200; ++slot) {
        for (u32 k = 0; k < kMeteorMaxPerSlot; ++k) {
            const MeteorStreak s = meteor_make(m, slot, k);
            const math::vec3 normal = cross3(s.start, s.travel);
            math::vec3 prev = meteor_head(s, 0.0f);
            CHECK(dot3(prev, s.start) == doctest::Approx(1.0f).epsilon(1e-5));
            for (u32 i = 1; i <= 10; ++i) {
                const f32 age = s.duration * static_cast<f32>(i) / 10.0f;
                const math::vec3 h = meteor_head(s, age);
                const math::vec3 t = meteor_tail(s, age);
                CHECK(dot3(h, h) == doctest::Approx(1.0f).epsilon(1e-5));
                CHECK(dot3(t, t) == doctest::Approx(1.0f).epsilon(1e-5));
                // the head, the tail and the start share the plane of the great circle
                CHECK(std::fabs(dot3(h, normal)) < 1e-5f);
                CHECK(std::fabs(dot3(t, normal)) < 1e-5f);
                // the head advances by angular_speed * dt, the tail trails it by tail_angle
                const f32 step = std::acos(std::clamp(dot3(prev, h), -1.0f, 1.0f));
                CHECK(step == doctest::Approx(s.angular_speed * s.duration / 10.0f).epsilon(0.01));
                CHECK(std::acos(std::clamp(dot3(h, t), -1.0f, 1.0f)) == doctest::Approx(s.tail_angle).epsilon(0.001));
                prev = h;
            }
            // the age is clamped to the streak, NaN counts as 0
            CHECK(same_vec(meteor_head(s, -3.0f), meteor_head(s, 0.0f)));
            CHECK(same_vec(meteor_head(s, 99.0f), meteor_head(s, s.duration)));
            CHECK(same_vec(meteor_head(s, kNaN), meteor_head(s, 0.0f)));
        }
    }
}

TEST_CASE("meteor: the envelope is 0 outside the streak and peaks inside") {
    Meteor m;
    m.rate = 30.0f;
    MeteorStreak s = meteor_make(m, 4, 0);
    REQUIRE(s.active);
    CHECK(meteor_envelope(s, -0.01f) == 0.0f);
    CHECK(meteor_envelope(s, 0.0f) == 0.0f);
    CHECK(meteor_envelope(s, s.duration) == 0.0f);
    CHECK(meteor_envelope(s, s.duration + 0.01f) == 0.0f);
    CHECK(meteor_envelope(s, kNaN) == 0.0f);
    CHECK(meteor_envelope(s, kInf) == 0.0f);
    f32 peak = 0.0f, peak_x = 0.0f;
    for (u32 i = 0; i <= 200; ++i) {
        const f32 x = static_cast<f32>(i) / 200.0f;
        const f32 e = meteor_envelope(s, x * s.duration);
        CHECK(e >= 0.0f);
        CHECK(e <= 1.0f);
        if (e > peak) {
            peak = e;
            peak_x = x;
        }
    }
    CHECK(peak == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(peak_x > 0.05f);
    CHECK(peak_x < 0.3f);  // a fast rise, a slow fall
    CHECK(meteor_envelope(s, 0.5f * s.duration) > meteor_envelope(s, 0.05f * s.duration));
    // an inactive streak has no envelope
    s.active = false;
    CHECK(meteor_envelope(s, 0.5f * s.duration) == 0.0f);
}

TEST_CASE("meteor: slots and wrapped time repeat after 60 seconds") {
    CHECK(meteor_wrap_time(0.0) == 0.0f);
    CHECK(meteor_wrap_time(60.0) == 0.0f);
    CHECK(meteor_wrap_time(61.5) == doctest::Approx(1.5f));
    CHECK(meteor_wrap_time(-1.0) == doctest::Approx(59.0f));
    CHECK(meteor_wrap_time(6.0e9 + 12.25) == doctest::Approx(static_cast<f32>(std::fmod(6.0e9 + 12.25, 60.0))));
    CHECK(meteor_wrap_time(static_cast<f64>(kNaN)) == 0.0f);
    CHECK(meteor_wrap_time(static_cast<f64>(kInf)) == 0.0f);
    CHECK(meteor_wrap_time(-static_cast<f64>(kInf)) == 0.0f);
    for (f64 t : {0.0, 3.3, 17.9, 59.99, 123.456}) {
        const f32 w = meteor_wrap_time(t);
        CHECK(w >= 0.0f);
        CHECK(w < 60.0f);
    }
    CHECK(meteor_slot(0.0f).slot == 0);
    CHECK(meteor_slot(4.99f).slot == 0);
    CHECK(meteor_slot(5.0f).slot == 1);
    CHECK(meteor_slot(5.0f).frac == 0.0f);
    CHECK(meteor_slot(7.5f).frac == doctest::Approx(0.5f));
    CHECK(meteor_slot(59.999f).slot == 11);
    CHECK(meteor_slot(60.0f).slot == 11);
    CHECK(meteor_slot(kNaN).slot == 0);
    CHECK(meteor_slot(-5.0f).slot == 0);
    Meteor m;
    m.rate = 20.0f;
    m.seed = 3;
    // the streaks of a slot, derived from the wrapped time, repeat exactly after a period and at +10 periods
    for (f64 t = 0.0; t < 60.0; t += 0.37) {
        const MeteorSlot a = meteor_slot(meteor_wrap_time(t));
        const MeteorSlot b = meteor_slot(meteor_wrap_time(t + 60.0));
        const MeteorSlot c = meteor_slot(meteor_wrap_time(t + 600.0));
        CHECK(a.slot == b.slot);
        CHECK(a.slot == c.slot);
        CHECK(doctest::Approx(a.frac).epsilon(1e-3) == b.frac);
        CHECK(doctest::Approx(a.frac).epsilon(1e-3) == c.frac);
        for (u32 k = 0; k < kMeteorMaxPerSlot; ++k) {
            CHECK(same_streak(meteor_make(m, a.slot, k), meteor_make(m, b.slot, k)));
            CHECK(same_streak(meteor_make(m, a.slot, k), meteor_make(m, c.slot, k)));
        }
    }
    // slot n equals slot n + 12 when derived from the wrapped time (the caller reduces the slot with the period)
    for (u32 slot = 0; slot < 12; ++slot) CHECK(meteor_slot(static_cast<f32>(slot) * 5.0f + 1.0f).slot == slot);
}

TEST_CASE("meteor: NaN, inverted and zero ranges fall back or clamp") {
    const Meteor ref;
    Meteor n;
    n.rate = kNaN;
    n.brightness = kNaN;
    n.length = kNaN;
    n.speed = kNaN;
    n.min_darkness = kNaN;
    CHECK(night_level(n) == night_level(ref));
    CHECK(meteor_level(n, 0.8f, 0.0f, 0.0f) == meteor_level(ref, 0.8f, 0.0f, 0.0f));
    for (u32 slot = 0; slot < 40; ++slot)
        for (u32 k = 0; k < kMeteorMaxPerSlot; ++k) CHECK(same_streak(meteor_make(n, slot, k), meteor_make(ref, slot, k)));
    // zero length and speed are outside their range: the default
    Meteor z;
    z.length = 0.0f;
    z.speed = 0.0f;
    for (u32 slot = 0; slot < 40; ++slot)
        for (u32 k = 0; k < kMeteorMaxPerSlot; ++k) CHECK(same_streak(meteor_make(z, slot, k), meteor_make(ref, slot, k)));
    Meteor neg = z;
    neg.length = -2.0f;
    neg.speed = -1.0f;
    CHECK(same_streak(meteor_make(neg, 5, 0), meteor_make(ref, 5, 0)));
    // out of range clamps
    Meteor big;
    big.rate = 1000.0f;
    big.length = 50.0f;
    big.speed = 50.0f;
    big.brightness = 50.0f;
    big.seed = 0xFFFFFFFFu;
    Meteor cap;
    cap.rate = 30.0f;
    cap.length = 2.0f;
    cap.speed = 2.0f;
    cap.brightness = 4.0f;
    cap.seed = 65535u;
    for (u32 slot = 0; slot < 40; ++slot) {
        CHECK(meteor_slot_count(big, slot) <= kMeteorMaxPerSlot);
        for (u32 k = 0; k < kMeteorMaxPerSlot; ++k) CHECK(same_streak(meteor_make(big, slot, k), meteor_make(cap, slot, k)));
    }
    Meteor low;
    low.length = 0.01f;
    low.speed = 0.01f;
    CHECK(meteor_make(low, 2, 0).tail_angle == doctest::Approx(0.5f * 8.0f * 0.017453292f));
    // min_darkness: 1 is a step at full night; the inputs clamp and NaN counts as 0
    Meteor step;
    step.min_darkness = 1.0f;
    CHECK(meteor_level(step, 0.99f, 0.0f, 0.0f) == 0.0f);
    CHECK(meteor_level(step, 1.0f, 0.0f, 0.0f) == 1.0f);
    CHECK(meteor_level(ref, kNaN, 0.0f, 0.0f) == 0.0f);
    CHECK(meteor_level(ref, 1.0f, kNaN, kNaN) == 1.0f);
    CHECK(meteor_level(ref, 5.0f, -3.0f, -3.0f) == 1.0f);
    CHECK(meteor_level(ref, -5.0f, 0.0f, 0.0f) == 0.0f);
    CHECK(meteor_level(ref, 1.0f, 7.0f, 0.0f) == 0.0f);
    CHECK(meteor_level(ref, 1.0f, 0.0f, 7.0f) == 0.0f);
    Meteor nanrate;
    nanrate.rate = -4.0f;  // negative clamps to 0: nothing drawn
    CHECK(night_level(nanrate) == 0.0f);
    CHECK(meteor_slot_count(nanrate, 0) == 0);
    // an empty / inactive streak is harmless
    const MeteorStreak none;
    CHECK(meteor_envelope(none, 0.1f) == 0.0f);
}

namespace {

struct Rig {
    World w;
    u64 t = 0;
    Entity wxe, mete, ce, me, ae, re;
    explicit Rig(bool with_meteor, f32 hour = 22.0f) {
        w.register_components<SaveKey>();
        install_weather(w, nullptr);
        if (Physics3D* p = w.try_resource<Physics3D>()) p->auto_step = 0;
        Weather c;
        c.target = WeatherKind::Rain;
        c.transition_seconds = 0.5f;
        wxe = w.spawn_with(SaveKey{1}, c);
        if (with_meteor) {
            Meteor m;
            m.enabled = 0;
            mete = w.spawn_with(SaveKey{2}, m);
        }
        WorldClock k;
        k.hour = hour;
        k.day_length_seconds = 20.0f;
        ce = w.spawn_with(SaveKey{3}, k);
        me = w.spawn_with(SaveKey{4}, Moon{});
        ae = w.spawn_with(SaveKey{5}, Aurora{});
        re = w.spawn_with(SaveKey{6}, Rainbow{});
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    // The helpers fed from the live weather state and the tick time, packed into bits for an exact comparison.
    std::vector<u32> sample(const Meteor& m) {
        const Weather* wx = w.try_get<Weather>(wxe);
        const WorldClock* k = w.try_get<WorldClock>(ce);
        const f32 fade = std::clamp(0.5f - 0.5f * std::sin((k->hour - 6.0f) * 0.2617994f) * 4.0f, 0.0f, 1.0f);
        std::vector<u32> bits;
        bits.push_back(std::bit_cast<u32>(meteor_level(m, fade, 0.1f, wx->intensity)));
        const MeteorSlot sl = meteor_slot(meteor_wrap_time(static_cast<f64>(t) * static_cast<f64>(kDt) * 4.0));
        bits.push_back(sl.slot);
        bits.push_back(std::bit_cast<u32>(sl.frac));
        for (u32 i = 0; i < kMeteorMaxPerSlot; ++i) {
            const MeteorStreak s = meteor_make(m, sl.slot, i);
            const math::vec3 h = meteor_head(s, sl.frac * kMeteorSlotSeconds - s.start_time);
            bits.push_back(s.active ? 1u : 0u);
            bits.push_back(std::bit_cast<u32>(h.x));
            bits.push_back(std::bit_cast<u32>(h.y));
            bits.push_back(std::bit_cast<u32>(h.z));
            bits.push_back(std::bit_cast<u32>(meteor_envelope(s, sl.frac * kMeteorSlotSeconds - s.start_time)));
        }
        return bits;
    }
};

}  // namespace

TEST_CASE("meteor: the component round-trips save and load") {
    Array<byte> bytes;
    u64 hash_a = 0;
    {
        Rig a(true);
        Meteor& m = *a.w.try_get_mut<Meteor>(a.mete);
        m.enabled = 1;
        m.rate = 12.5f;
        m.brightness = 2.5f;
        m.length = 1.5f;
        m.speed = 0.75f;
        m.min_darkness = 0.4f;
        m.night_only = 0;
        m.seed = 4242;
        a.run(30);
        save::write_save(a.w, {}, bytes);
        hash_a = a.w.state_hash();
    }
    Rig b(true);
    b.run(5);
    REQUIRE(save::read_save(b.w, bytes.span()));
    const Meteor& m = *b.w.try_get<Meteor>(b.mete);
    CHECK(m.enabled == 1);
    CHECK(m.rate == 12.5f);
    CHECK(m.brightness == 2.5f);
    CHECK(m.length == 1.5f);
    CHECK(m.speed == 0.75f);
    CHECK(m.min_darkness == 0.4f);
    CHECK(m.night_only == 0);
    CHECK(m.seed == 4242);
    CHECK(m.pad_[0] == 0);
    CHECK(m.pad_[1] == 0);
    CHECK(b.w.state_hash() == hash_a);
}

TEST_CASE("meteor: the Weather, WorldClock, Moon, Aurora and Rainbow data are the same with and without a Meteor") {
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
        const Rainbow& ra = *a.w.try_get<Rainbow>(a.re);
        const Rainbow& rb = *b.w.try_get<Rainbow>(b.re);
        CHECK(same_bits(ra.strength, rb.strength));
        CHECK(same_bits(ra.band_width, rb.band_width));
        CHECK(ra.enabled == rb.enabled);
    }
    // the meteor code never writes the component either: it is still as authored
    const Meteor& m = *b.w.try_get<Meteor>(b.mete);
    CHECK(m.rate == 4.0f);
    CHECK(m.enabled == 0);
    Rig c(false), d(false);
    c.run(300);
    d.run(300);
    CHECK(c.w.state_hash() == d.w.state_hash());
}

TEST_CASE("meteor: the helpers at fixed ticks are the same in one go and in two halves") {
    constexpr u32 kN = 600;
    constexpr u32 kEvery = 20;
    Meteor authored;
    authored.rate = 24.0f;
    authored.seed = 9;
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
    for (usize i = 1; i < whole.size(); ++i) moves = moves || whole[i][2] != whole[i - 1][2];
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
