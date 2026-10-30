// WaterIce (M-FREEZE 20a): the amount is 0 before freeze_start and 1 from freeze_full, monotone and continuous between;
// `manual` is a floor and works with no Season; summer is exactly 0 with manual 0; the look is the exact identity at amount
// 0 and flat waves at amount 1 with calm 1; strength 0, enabled 0 and a missing component give the identity exactly; NaN,
// inverted and zero ranges fall back; the helpers agree over a WorldClock run in one go and in two halves; the component
// round-trips save/load; the Water, Season and WorldClock columns are the same with and without a WaterIce.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>
#include <vector>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/season.h"
#include "mce/kit/water.h"
#include "mce/kit/water_ice.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_vec(math::vec3 a, math::vec3 b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }

bool is_identity(const WaterIceLook& l) {
    return same_bits(l.amplitude_scale, 1.0f) && same_bits(l.steepness_scale, 1.0f) && same_bits(l.speed_scale, 1.0f) &&
           same_bits(l.ice_mix, 0.0f) && same_bits(l.roughness_floor, 0.0f) && same_bits(l.reflectivity_scale, 1.0f) &&
           same_bits(l.foam_scale, 1.0f);
}

// A Season whose winter weight is the phase itself between 0.5 and 0.875 is not linear, so tests pick phases directly.
const Season kSeason;

}  // namespace

TEST_CASE("water_ice: defaults") {
    const WaterIce w;
    CHECK(w.enabled == 1);
    CHECK(w.freeze_start == 0.5f);
    CHECK(w.freeze_full == 0.9f);
    CHECK(w.manual == 0.0f);
    CHECK(w.ice_roughness == 0.35f);
    CHECK(w.ice_reflectivity == 0.55f);
    CHECK(w.ice_depth_m == 0.4f);
    CHECK(w.calm == 1.0f);
    CHECK(w.strength == 1.0f);
    CHECK(same_vec(w.ice_color, math::vec3{0.62f, 0.78f, 0.85f}));
    CHECK(w.pad_[0] == 0);
    CHECK(w.pad_[1] == 0);
    CHECK(w.pad_[2] == 0);
}

TEST_CASE("water_ice: the amount is 0 before freeze_start and 1 from freeze_full") {
    const WaterIce w;
    // the winter weight is 0 until phase 0.5 and 1 at the centre of winter
    for (f32 p = 0.0f; p <= 0.5f; p += 0.01f) CHECK(water_ice_amount(w, &kSeason, p) == 0.0f);
    CHECK(water_ice_amount(w, &kSeason, kSeasonWinterCentre) == 1.0f);
    CHECK(water_ice_amount(w, &kSeason, 0.92f) > 0.0f);
    // a narrow window: start and full on the winter weight itself
    WaterIce n;
    n.freeze_start = 0.2f;
    n.freeze_full = 0.6f;
    CHECK(water_ice_amount(n, &kSeason, 0.5f) == 0.0f);
    CHECK(water_ice_amount(n, &kSeason, kSeasonWinterCentre) == 1.0f);
    const f32 mid = water_ice_amount(n, &kSeason, 0.65f);
    CHECK(mid > 0.0f);
    CHECK(mid < 1.0f);
}

TEST_CASE("water_ice: the amount is monotone and continuous between the thresholds") {
    const WaterIce w;
    f32 prev = water_ice_amount(w, &kSeason, 0.5f);
    bool saw_between = false;
    for (u32 i = 1; i <= 3750; ++i) {
        const f32 p = 0.5f + static_cast<f32>(i) * 0.0001f;  // up to the centre of winter
        const f32 a = water_ice_amount(w, &kSeason, p);
        CHECK(a >= prev);
        CHECK(a - prev < 0.01f);  // no jump
        saw_between = saw_between || (a > 0.0f && a < 1.0f);
        prev = a;
    }
    CHECK(saw_between);
    CHECK(prev == 1.0f);
    // falling out of winter is monotone down to the year wrap
    for (u32 i = 0; i < 1250; ++i) {
        const f32 p = kSeasonWinterCentre + static_cast<f32>(i) * 0.0001f;
        const f32 a = water_ice_amount(w, &kSeason, p);
        CHECK(a <= prev);
        CHECK(prev - a < 0.01f);
        prev = a;
    }
    // continuous across the year wrap
    CHECK(std::fabs(water_ice_amount(w, &kSeason, 0.99999f) - water_ice_amount(w, &kSeason, 0.0f)) < 0.01f);
}

TEST_CASE("water_ice: manual is a floor and works with no Season") {
    WaterIce w;
    w.manual = 0.6f;
    CHECK(water_ice_amount(w, nullptr, 0.9f) == 0.6f);
    CHECK(water_ice_amount(w, nullptr, 0.0f) == 0.6f);
    CHECK(water_ice_amount(w, &kSeason, 0.3f) == 0.6f);  // summer: the floor
    CHECK(water_ice_amount(w, &kSeason, kSeasonWinterCentre) == 1.0f);  // winter beats the floor
    w.manual = 1.0f;
    CHECK(water_ice_amount(w, nullptr, 0.0f) == 1.0f);
    // no Season and manual 0: nothing freezes, whatever the phase
    const WaterIce d;
    CHECK(water_ice_amount(d, nullptr, kSeasonWinterCentre) == 0.0f);
    // a disabled Season is no Season
    Season off;
    off.enabled = 0;
    CHECK(water_ice_amount(d, &off, kSeasonWinterCentre) == 0.0f);
    Season weak;
    weak.strength = 0.0f;
    CHECK(water_ice_amount(d, &weak, kSeasonWinterCentre) == 0.0f);
}

TEST_CASE("water_ice: summer is exactly 0 with manual 0") {
    const WaterIce w;
    for (f32 p = 0.0f; p < 0.5f; p += 0.005f) CHECK(water_ice_amount(w, &kSeason, p) == 0.0f);
    CHECK(water_ice_amount(w, &kSeason, kSeasonSummerCentre) == 0.0f);
}

TEST_CASE("water_ice: the look is the exact identity at amount 0") {
    const WaterIce w;
    const WaterIceLook l = water_ice_look(w, 0.0f);
    CHECK(is_identity(l));
    CHECK(l.depth_scale_m == 0.4f);
    // the amount-0 look from the helper chain in summer
    CHECK(is_identity(water_ice_look(w, water_ice_amount(w, &kSeason, kSeasonSummerCentre))));
}

TEST_CASE("water_ice: the look flattens the waves at amount 1 with calm 1") {
    const WaterIce w;
    const WaterIceLook l = water_ice_look(w, 1.0f);
    CHECK(l.amplitude_scale == 0.0f);
    CHECK(l.steepness_scale == 0.0f);
    CHECK(l.speed_scale == 0.0f);
    CHECK(l.ice_mix == 1.0f);
    CHECK(l.roughness_floor == 0.35f);
    CHECK(l.reflectivity_scale == doctest::Approx(0.55f));
    CHECK(l.depth_scale_m == 0.4f);
    CHECK(l.foam_scale == 0.0f);
    // half way, calm 0.5
    WaterIce h;
    h.calm = 0.5f;
    const WaterIceLook m = water_ice_look(h, 0.5f);
    CHECK(m.amplitude_scale == doctest::Approx(0.75f));
    CHECK(m.ice_mix == 0.5f);
    CHECK(m.roughness_floor == doctest::Approx(0.175f));
    CHECK(m.reflectivity_scale == doctest::Approx(1.0f - 0.5f * 0.45f));
    CHECK(m.foam_scale == 0.5f);
    // calm 0 leaves the waves alone even when frozen
    WaterIce c;
    c.calm = 0.0f;
    CHECK(water_ice_look(c, 1.0f).amplitude_scale == 1.0f);
    // the colour helper clamps
    WaterIce col;
    col.ice_color = math::vec3{2.0f, -1.0f, kNaN};
    CHECK(same_vec(water_ice_color(col), math::vec3{1.0f, 0.0f, 0.85f}));
    CHECK(same_vec(water_ice_color(WaterIce{}), math::vec3{0.62f, 0.78f, 0.85f}));
}

TEST_CASE("water_ice: strength 0, enabled 0 and a missing component give the identity exactly") {
    WaterIce s0;
    s0.strength = 0.0f;
    s0.manual = 1.0f;
    CHECK(water_ice_amount(s0, &kSeason, kSeasonWinterCentre) == 0.0f);
    CHECK(is_identity(water_ice_look(s0, 1.0f)));
    WaterIce e0;
    e0.enabled = 0;
    e0.manual = 1.0f;
    CHECK(water_ice_amount(e0, &kSeason, kSeasonWinterCentre) == 0.0f);
    CHECK(is_identity(water_ice_look(e0, 1.0f)));
    const WaterIce* none = nullptr;
    CHECK(water_ice_amount(none, &kSeason, kSeasonWinterCentre) == 0.0f);
    CHECK(is_identity(water_ice_look(none, 1.0f)));
    // strength scales the amount
    WaterIce half;
    half.strength = 0.5f;
    CHECK(water_ice_amount(half, &kSeason, kSeasonWinterCentre) == 0.5f);
}

TEST_CASE("water_ice: NaN, inverted and zero ranges fall back") {
    const WaterIce ref;
    const f32 p = 0.7f;
    const f32 want = water_ice_amount(ref, &kSeason, p);
    WaterIce n;
    n.freeze_start = kNaN;
    n.freeze_full = kNaN;
    CHECK(same_bits(water_ice_amount(n, &kSeason, p), want));
    WaterIce z;  // an empty range
    z.freeze_start = 0.7f;
    z.freeze_full = 0.7f;
    CHECK(same_bits(water_ice_amount(z, &kSeason, p), want));
    WaterIce inv;  // inverted: swapped
    inv.freeze_start = 0.9f;
    inv.freeze_full = 0.5f;
    CHECK(same_bits(water_ice_amount(inv, &kSeason, p), want));
    WaterIce nm;
    nm.manual = kNaN;
    CHECK(water_ice_amount(nm, nullptr, p) == 0.0f);
    WaterIce ns;
    ns.strength = kNaN;
    CHECK(same_bits(water_ice_amount(ns, &kSeason, p), want));
    // out of range clamps
    WaterIce big;
    big.manual = 9.0f;
    big.strength = 7.0f;
    CHECK(water_ice_amount(big, nullptr, 0.0f) == 1.0f);
    // NaN phase counts as summer
    CHECK(water_ice_amount(ref, &kSeason, kNaN) == 0.0f);
    // look: NaN amount is 0, out of range amount clamps, bad fields fall back
    CHECK(is_identity(water_ice_look(ref, kNaN)));
    CHECK(water_ice_look(ref, 5.0f).ice_mix == 1.0f);
    CHECK(water_ice_look(ref, -5.0f).ice_mix == 0.0f);
    WaterIce bad;
    bad.ice_depth_m = kNaN;
    bad.calm = kNaN;
    bad.ice_roughness = kNaN;
    bad.ice_reflectivity = kNaN;
    const WaterIceLook lb = water_ice_look(bad, 1.0f);
    CHECK(lb.depth_scale_m == 0.4f);
    CHECK(lb.amplitude_scale == 0.0f);
    CHECK(lb.roughness_floor == 0.35f);
    CHECK(lb.reflectivity_scale == doctest::Approx(0.55f));
    WaterIce deep;
    deep.ice_depth_m = 100.0f;
    CHECK(water_ice_look(deep, 1.0f).depth_scale_m == 10.0f);
    deep.ice_depth_m = 0.0f;
    CHECK(water_ice_look(deep, 1.0f).depth_scale_m == 0.05f);
}

namespace {

struct Rig {
    World w;
    u64 t = 0;
    Entity we, ce, se, ie;
    explicit Rig(bool with_ice, f32 day_length = 4.0f) {
        w.register_components<SaveKey>();
        install_weather(w, nullptr);
        if (Physics3D* p = w.try_resource<Physics3D>()) p->auto_step = 0;
        we = w.spawn_with(SaveKey{1}, Water{});
        WorldClock k;
        k.hour = 6.0f;
        k.day_length_seconds = day_length;
        ce = w.spawn_with(SaveKey{2}, k);
        Season s;
        s.year_days = 3.0f;
        s.start_phase = 0.7f;
        se = w.spawn_with(SaveKey{3}, s);
        if (with_ice) {
            WaterIce i;
            i.enabled = 0;
            ie = w.spawn_with(SaveKey{4}, i);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    // The helpers fed from the live clock, packed into bits for an exact comparison.
    std::vector<u32> sample(const WaterIce& ice) const {
        const WorldClock* k = w.try_get<WorldClock>(ce);
        const Season* s = w.try_get<Season>(se);
        const f32 p = season_phase(*s, k->day, k->hour);
        const f32 a = water_ice_amount(ice, s, p);
        const WaterIceLook l = water_ice_look(ice, a);
        return {k->day,
                std::bit_cast<u32>(p),
                std::bit_cast<u32>(a),
                std::bit_cast<u32>(l.amplitude_scale),
                std::bit_cast<u32>(l.ice_mix),
                std::bit_cast<u32>(l.roughness_floor),
                std::bit_cast<u32>(l.reflectivity_scale),
                std::bit_cast<u32>(l.foam_scale)};
    }
};

}  // namespace

TEST_CASE("water_ice: the component round-trips save and load") {
    Array<byte> bytes;
    u64 hash_a = 0;
    {
        Rig a(true);
        WaterIce& i = *a.w.try_get_mut<WaterIce>(a.ie);
        i.enabled = 1;
        i.freeze_start = 0.3f;
        i.freeze_full = 0.7f;
        i.manual = 0.2f;
        i.ice_color = math::vec3{0.5f, 0.6f, 0.9f};
        i.ice_roughness = 0.6f;
        i.ice_reflectivity = 0.25f;
        i.ice_depth_m = 1.5f;
        i.calm = 0.8f;
        i.strength = 0.9f;
        a.run(30);
        save::write_save(a.w, {}, bytes);
        hash_a = a.w.state_hash();
    }
    Rig b(true);
    b.run(5);
    REQUIRE(save::read_save(b.w, bytes.span()));
    const WaterIce& i = *b.w.try_get<WaterIce>(b.ie);
    CHECK(i.enabled == 1);
    CHECK(i.freeze_start == 0.3f);
    CHECK(i.freeze_full == 0.7f);
    CHECK(i.manual == 0.2f);
    CHECK(same_vec(i.ice_color, math::vec3{0.5f, 0.6f, 0.9f}));
    CHECK(i.ice_roughness == 0.6f);
    CHECK(i.ice_reflectivity == 0.25f);
    CHECK(i.ice_depth_m == 1.5f);
    CHECK(i.calm == 0.8f);
    CHECK(i.strength == 0.9f);
    CHECK(i.pad_[0] == 0);
    CHECK(i.pad_[1] == 0);
    CHECK(i.pad_[2] == 0);
    CHECK(b.w.state_hash() == hash_a);
}

TEST_CASE("water_ice: the Water, Season and WorldClock data are the same with and without a WaterIce") {
    Rig a(false), b(true);
    for (u32 i = 0; i < 300; ++i) {
        a.run(1);
        b.run(1);
        const Water& wa = *a.w.try_get<Water>(a.we);
        const Water& wb = *b.w.try_get<Water>(b.we);
        CHECK(same_bits(wa.level_y, wb.level_y));
        CHECK(same_bits(wa.amplitude, wb.amplitude));
        CHECK(same_bits(wa.steepness, wb.steepness));
        CHECK(same_bits(wa.speed_scale, wb.speed_scale));
        CHECK(same_vec(wa.shallow_color, wb.shallow_color));
        CHECK(same_vec(wa.deep_color, wb.deep_color));
        CHECK(same_bits(wa.depth_scale_m, wb.depth_scale_m));
        CHECK(same_bits(wa.roughness, wb.roughness));
        CHECK(same_bits(wa.reflectivity, wb.reflectivity));
        CHECK(same_bits(wa.foam_amount, wb.foam_amount));
        const WorldClock& ka = *a.w.try_get<WorldClock>(a.ce);
        const WorldClock& kb = *b.w.try_get<WorldClock>(b.ce);
        CHECK(same_bits(ka.hour, kb.hour));
        CHECK(ka.day == kb.day);
        CHECK(ka.phase == kb.phase);
        CHECK(ka.phase_den == kb.phase_den);
        const Season& sa = *a.w.try_get<Season>(a.se);
        const Season& sb = *b.w.try_get<Season>(b.se);
        CHECK(same_bits(sa.year_days, sb.year_days));
        CHECK(same_bits(sa.start_phase, sb.start_phase));
        CHECK(same_bits(sa.strength, sb.strength));
    }
    // the ice code never writes the component: it is still as authored
    const WaterIce& ice = *b.w.try_get<WaterIce>(b.ie);
    CHECK(ice.enabled == 0);
    CHECK(ice.freeze_start == 0.5f);
    Rig c(false), d(false);
    c.run(300);
    d.run(300);
    CHECK(c.w.state_hash() == d.w.state_hash());
}

TEST_CASE("water_ice: the helpers over a WorldClock run are the same in one go and in two halves") {
    constexpr u32 kN = 1200;
    constexpr u32 kEvery = 20;
    WaterIce authored;
    authored.freeze_start = 0.2f;
    authored.freeze_full = 0.8f;
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
    CHECK(whole.back()[0] > whole.front()[0]);
    std::vector<std::vector<u32>> split;
    Array<byte> bytes;
    {
        Rig a(true);
        for (u32 i = 0; i < kN / 2; ++i) {
            a.run(1);
            if ((i + 1) % kEvery == 0) split.push_back(a.sample(authored));
        }
        save::write_save(a.w, {}, bytes);
    }
    {
        Rig b(true);
        b.run(3);
        REQUIRE(save::read_save(b.w, bytes.span()));
        b.t = kN / 2;
        for (u32 i = kN / 2; i < kN; ++i) {
            b.run(1);
            if ((i + 1) % kEvery == 0) split.push_back(b.sample(authored));
        }
    }
    REQUIRE(split.size() == whole.size());
    for (usize i = 0; i < whole.size(); ++i) CHECK(whole[i] == split[i]);
}
