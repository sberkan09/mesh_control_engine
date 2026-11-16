// LeafFall (M-LEAFFALL 21a): the amount is 0 before autumn_start and after autumn_end and 1 across the plateau, continuous
// and symmetric about the autumn centre; `manual` is a floor and works with no Season; enabled 0, strength 0 and a missing
// component give 0 exactly; the look is inactive zeros at amount 0, its terminal speed is the authored fall speed, the drift
// follows the wind and the colour follows the tint; NaN, inverted and zero ranges fall back; the helpers agree over a
// WorldClock run in one go and in two halves; the component round-trips save/load; the Season, WorldClock and Weather data
// are the same with and without a LeafFall.

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>
#include <vector>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/leaf_fall.h"
#include "mce/kit/season.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_vec(math::vec3 a, math::vec3 b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }

bool is_zero_look(const LeafFallLook& l) {
    return !l.active && same_bits(l.rate, 0.0f) && same_bits(l.speed_min, 0.0f) && same_bits(l.speed_max, 0.0f) &&
           same_vec(l.direction, math::vec3{0.0f, 0.0f, 0.0f}) && same_bits(l.spread, 0.0f) &&
           same_vec(l.gravity, math::vec3{0.0f, 0.0f, 0.0f}) && same_bits(l.drag, 0.0f) && same_bits(l.size, 0.0f) &&
           same_bits(l.lifetime_min, 0.0f) && same_bits(l.lifetime_max, 0.0f) && same_bits(l.color_start.w, 0.0f) &&
           same_bits(l.color_start.x, 0.0f) && same_bits(l.color_end.w, 0.0f) && same_bits(l.box, 0.0f) &&
           same_bits(l.height, 0.0f);
}

const Season kSeason;
const math::vec2 kEast{1.0f, 0.0f};
const math::vec3 kWhite{1.0f, 1.0f, 1.0f};

}  // namespace

TEST_CASE("leaf_fall: defaults") {
    const LeafFall l;
    CHECK(l.enabled == 1);
    CHECK(l.autumn_start == 0.5f);
    CHECK(l.autumn_full == 0.6f);
    CHECK(l.autumn_end_full == 0.65f);
    CHECK(l.autumn_end == 0.75f);
    CHECK(l.manual == 0.0f);
    CHECK(l.rate == 24.0f);
    CHECK(l.box_xz_m == 14.0f);
    CHECK(l.height_m == 7.0f);
    CHECK(l.size_m == 0.14f);
    CHECK(l.lifetime_s == 7.0f);
    CHECK(l.fall_speed == 0.9f);
    CHECK(l.wind_follow == 1.0f);
    CHECK(l.tint_follow == 1.0f);
    CHECK(l.max_particles == 1024);
    CHECK(l.seed == 1);
    CHECK(l.strength == 1.0f);
    CHECK(same_vec(l.leaf_color, math::vec3{0.95f, 0.5f, 0.15f}));
    CHECK(l.pad_[0] == 0);
    CHECK(l.pad_[1] == 0);
    CHECK(l.pad_[2] == 0);
}

TEST_CASE("leaf_fall: the window is 0 outside autumn, 1 across the plateau and continuous") {
    const LeafFall l;
    for (f32 p = 0.0f; p <= 0.5f; p += 0.01f) CHECK(leaf_fall_amount(l, &kSeason, p) == 0.0f);
    for (f32 p = 0.75f; p < 1.0f; p += 0.01f) CHECK(leaf_fall_amount(l, &kSeason, p) == 0.0f);
    CHECK(leaf_fall_amount(l, &kSeason, 0.6f) == 1.0f);
    CHECK(leaf_fall_amount(l, &kSeason, kSeasonAutumnCentre) == 1.0f);
    CHECK(leaf_fall_amount(l, &kSeason, 0.65f) == 1.0f);
    const f32 up = leaf_fall_amount(l, &kSeason, 0.55f);
    CHECK(up > 0.0f);
    CHECK(up < 1.0f);
    const f32 down = leaf_fall_amount(l, &kSeason, 0.7f);
    CHECK(down > 0.0f);
    CHECK(down < 1.0f);
    // continuous across the four edges
    for (f32 edge : {0.5f, 0.6f, 0.65f, 0.75f}) {
        const f32 a = leaf_fall_amount(l, &kSeason, edge - 1e-4f);
        const f32 b = leaf_fall_amount(l, &kSeason, edge + 1e-4f);
        CHECK(std::fabs(a - b) < 1e-2f);
    }
    // monotone up then down
    f32 prev = 0.0f;
    for (f32 p = 0.5f; p <= 0.6f; p += 0.005f) {
        const f32 a = leaf_fall_amount(l, &kSeason, p);
        CHECK(a >= prev);
        prev = a;
    }
    for (f32 p = 0.65f; p <= 0.75f; p += 0.005f) {
        const f32 a = leaf_fall_amount(l, &kSeason, p);
        CHECK(a <= prev);
        prev = a;
    }
}

TEST_CASE("leaf_fall: the window is symmetric about the autumn centre") {
    const LeafFall l;
    for (f32 d = 0.0f; d <= 0.125f; d += 0.005f) {
        CHECK(leaf_fall_amount(l, &kSeason, kSeasonAutumnCentre - d) ==
              doctest::Approx(leaf_fall_amount(l, &kSeason, kSeasonAutumnCentre + d)).epsilon(1e-4));
    }
}

TEST_CASE("leaf_fall: the phase wraps and a NaN phase is summer") {
    const LeafFall l;
    CHECK(leaf_fall_amount(l, &kSeason, kSeasonAutumnCentre + 1.0f) == 1.0f);
    CHECK(leaf_fall_amount(l, &kSeason, kSeasonAutumnCentre - 1.0f) == 1.0f);
    CHECK(leaf_fall_amount(l, &kSeason, kNaN) == 0.0f);
}

TEST_CASE("leaf_fall: manual is a floor and works with no Season") {
    LeafFall l;
    l.manual = 0.4f;
    CHECK(leaf_fall_amount(l, nullptr, 0.1f) == 0.4f);
    CHECK(leaf_fall_amount(l, nullptr, kSeasonAutumnCentre) == 0.4f);
    CHECK(leaf_fall_amount(l, &kSeason, 0.1f) == 0.4f);
    CHECK(leaf_fall_amount(l, &kSeason, kSeasonAutumnCentre) == 1.0f);
    Season off;
    off.enabled = 0;
    CHECK(leaf_fall_amount(l, &off, kSeasonAutumnCentre) == 0.4f);
    // no Season and manual 0: nothing falls
    CHECK(leaf_fall_amount(LeafFall{}, nullptr, kSeasonAutumnCentre) == 0.0f);
    // a Season at strength 0 has no window
    Season zero;
    zero.strength = 0.0f;
    CHECK(leaf_fall_amount(LeafFall{}, &zero, kSeasonAutumnCentre) == 0.0f);
    // winter, spring and summer are exactly 0 with manual 0
    for (f32 p : {0.1f, 0.3f, 0.45f, 0.8f, 0.9f, 0.99f}) CHECK(leaf_fall_amount(LeafFall{}, &kSeason, p) == 0.0f);
}

TEST_CASE("leaf_fall: enabled 0, strength 0 and a missing component give exactly 0") {
    LeafFall off;
    off.enabled = 0;
    off.manual = 1.0f;
    CHECK(leaf_fall_amount(off, &kSeason, kSeasonAutumnCentre) == 0.0f);
    CHECK(is_zero_look(leaf_fall_look(off, 1.0f, 3.0f, kEast, kWhite)));
    LeafFall zero;
    zero.strength = 0.0f;
    zero.manual = 1.0f;
    CHECK(leaf_fall_amount(zero, &kSeason, kSeasonAutumnCentre) == 0.0f);
    CHECK(is_zero_look(leaf_fall_look(zero, 1.0f, 3.0f, kEast, kWhite)));
    const LeafFall* none = nullptr;
    CHECK(leaf_fall_amount(none, &kSeason, kSeasonAutumnCentre) == 0.0f);
    CHECK(is_zero_look(leaf_fall_look(none, 1.0f, 3.0f, kEast, kWhite)));
    LeafFall half;
    half.strength = 0.5f;
    CHECK(leaf_fall_amount(half, &kSeason, kSeasonAutumnCentre) == 0.5f);
}

TEST_CASE("leaf_fall: NaN, inverted and zero ranges fall back") {
    const LeafFall def;
    const f32 want = leaf_fall_amount(def, &kSeason, 0.55f);
    LeafFall n;
    n.autumn_start = kNaN;
    n.autumn_full = kNaN;
    n.autumn_end_full = kNaN;
    n.autumn_end = kNaN;
    n.manual = kNaN;
    n.strength = kNaN;
    CHECK(leaf_fall_amount(n, &kSeason, 0.55f) == want);
    LeafFall z;
    z.autumn_start = 0.55f;
    z.autumn_full = 0.55f;
    z.autumn_end_full = 0.7f;
    z.autumn_end = 0.7f;
    CHECK(leaf_fall_amount(z, &kSeason, 0.55f) == want);
    LeafFall inv;
    inv.autumn_start = 0.6f;
    inv.autumn_full = 0.5f;
    inv.autumn_end_full = 0.75f;
    inv.autumn_end = 0.65f;
    CHECK(leaf_fall_amount(inv, &kSeason, 0.55f) == want);
    CHECK(leaf_fall_amount(inv, &kSeason, 0.7f) == leaf_fall_amount(def, &kSeason, 0.7f));
    LeafFall big;
    big.manual = 9.0f;
    big.strength = 9.0f;
    CHECK(leaf_fall_amount(big, nullptr, 0.1f) == 1.0f);
    // out-of-range look inputs are clamped
    LeafFall wild;
    wild.rate = 9999.0f;
    wild.size_m = 100.0f;
    wild.lifetime_s = 1000.0f;
    wild.box_xz_m = 1000.0f;
    wild.height_m = 0.0f;
    const LeafFallLook w = leaf_fall_look(wild, 2.0f, kNaN, math::vec2{kNaN, kNaN}, kWhite);
    CHECK(w.rate == 500.0f);
    CHECK(w.size == 1.0f);
    CHECK(w.lifetime_max == 30.0f);
    CHECK(w.box == 100.0f);
    CHECK(w.height == 1.0f);
    CHECK(same_bits(w.direction.x, 0.0f));
    CHECK(same_bits(w.direction.z, 0.0f));
    CHECK(is_zero_look(leaf_fall_look(LeafFall{}, kNaN, 0.0f, kEast, kWhite)));
}

TEST_CASE("leaf_fall: the look is inactive zeros at amount 0 and live above it") {
    const LeafFall l;
    CHECK(is_zero_look(leaf_fall_look(l, 0.0f, 3.0f, kEast, kWhite)));
    CHECK(is_zero_look(leaf_fall_look(l, -1.0f, 3.0f, kEast, kWhite)));
    const LeafFallLook h = leaf_fall_look(l, 0.5f, 0.0f, kEast, kWhite);
    CHECK(h.active);
    CHECK(h.rate == doctest::Approx(12.0f));
    CHECK(h.size == 0.14f);
    CHECK(h.lifetime_max == 7.0f);
    CHECK(h.lifetime_min <= h.lifetime_max);
    CHECK(h.lifetime_min > 0.0f);
    CHECK(h.speed_min < h.speed_max);
    CHECK(h.spread > 0.0f);
    CHECK(h.box == 14.0f);
    CHECK(h.height == 7.0f);
    CHECK(h.color_start.w > 0.0f);
    CHECK(h.color_end.w == 0.0f);
}

TEST_CASE("leaf_fall: the terminal speed gravity over drag is the fall speed") {
    for (f32 fs : {0.2f, 0.9f, 2.5f, 5.0f}) {
        LeafFall l;
        l.fall_speed = fs;
        const LeafFallLook k = leaf_fall_look(l, 1.0f, 0.0f, kEast, kWhite);
        CHECK(k.gravity.y < 0.0f);
        CHECK(k.gravity.x == 0.0f);
        CHECK(k.gravity.z == 0.0f);
        CHECK(-k.gravity.y / k.drag == doctest::Approx(fs).epsilon(1e-5));
    }
    // a zero fall speed stays finite
    LeafFall z;
    z.fall_speed = 0.0f;
    const LeafFallLook k = leaf_fall_look(z, 1.0f, 0.0f, kEast, kWhite);
    CHECK(std::isfinite(k.drag));
    CHECK(k.drag > 0.0f);
}

TEST_CASE("leaf_fall: the drift follows the wind direction and is 0 at wind 0") {
    const LeafFall l;
    const LeafFallLook calm = leaf_fall_look(l, 1.0f, 0.0f, kEast, kWhite);
    CHECK(calm.direction.x == 0.0f);
    CHECK(calm.direction.z == 0.0f);
    CHECK(calm.direction.y == doctest::Approx(-1.0f));
    const LeafFallLook east = leaf_fall_look(l, 1.0f, 3.0f, kEast, kWhite);
    CHECK(east.direction.x > 0.0f);
    CHECK(east.direction.z == 0.0f);
    CHECK(east.direction.y < 0.0f);
    CHECK(math::length(east.direction) == doctest::Approx(1.0f));
    const LeafFallLook south = leaf_fall_look(l, 1.0f, 3.0f, math::vec2{0.0f, -1.0f}, kWhite);
    CHECK(south.direction.x == 0.0f);
    CHECK(south.direction.z < 0.0f);
    CHECK(east.speed_max > calm.speed_max);
    // wind_follow 0 turns the coupling off
    LeafFall nf;
    nf.wind_follow = 0.0f;
    const LeafFallLook off = leaf_fall_look(nf, 1.0f, 5.0f, kEast, kWhite);
    CHECK(off.direction.x == 0.0f);
    CHECK(off.speed_max == calm.speed_max);
    // a storm is capped so the drift stays bounded
    const LeafFallLook storm = leaf_fall_look(l, 1.0f, 500.0f, kEast, kWhite);
    CHECK(std::isfinite(storm.speed_max));
    CHECK(storm.speed_max <= leaf_fall_look(l, 1.0f, 6.0f, kEast, kWhite).speed_max);
}

TEST_CASE("leaf_fall: the colour is the leaf colour at tint_follow 0 and follows the tint at 1") {
    const math::vec3 tint{1.35f, 0.7f, 0.35f};
    LeafFall l;
    l.tint_follow = 0.0f;
    const LeafFallLook a = leaf_fall_look(l, 1.0f, 0.0f, kEast, tint);
    CHECK(a.color_start.x == 0.95f);
    CHECK(a.color_start.y == 0.5f);
    CHECK(a.color_start.z == 0.15f);
    l.tint_follow = 1.0f;
    const LeafFallLook b = leaf_fall_look(l, 1.0f, 0.0f, kEast, tint);
    CHECK(b.color_start.x == doctest::Approx(0.95f * 1.35f));
    CHECK(b.color_start.y == doctest::Approx(0.5f * 0.7f));
    CHECK(b.color_start.z == doctest::Approx(0.15f * 0.35f));
    l.tint_follow = 0.5f;
    const LeafFallLook c = leaf_fall_look(l, 1.0f, 0.0f, kEast, tint);
    CHECK(c.color_start.x == doctest::Approx(0.95f * 1.175f));
    CHECK(c.color_end.x == c.color_start.x);
    CHECK(c.color_end.y == c.color_start.y);
    CHECK(c.color_end.z == c.color_start.z);
}

TEST_CASE("leaf_fall: the helpers are deterministic") {
    LeafFall l;
    l.manual = 0.1f;
    for (f32 p = 0.0f; p < 1.0f; p += 0.013f) {
        const f32 a = leaf_fall_amount(l, &kSeason, p);
        const f32 b = leaf_fall_amount(l, &kSeason, p);
        CHECK(same_bits(a, b));
        const LeafFallLook x = leaf_fall_look(l, a, 2.0f, kEast, kWhite);
        const LeafFallLook y = leaf_fall_look(l, b, 2.0f, kEast, kWhite);
        CHECK(same_bits(x.rate, y.rate));
        CHECK(same_vec(x.direction, y.direction));
        CHECK(same_bits(x.color_start.w, y.color_start.w));
    }
}

namespace {

struct Rig {
    World w;
    u64 t = 0;
    Entity we, ce, se, le;
    explicit Rig(bool with_leaves, f32 day_length = 4.0f) {
        w.register_components<SaveKey>();
        install_weather(w, nullptr);
        if (Physics3D* p = w.try_resource<Physics3D>()) p->auto_step = 0;
        we = w.spawn_with(SaveKey{1}, Weather{});
        WorldClock k;
        k.hour = 6.0f;
        k.day_length_seconds = day_length;
        ce = w.spawn_with(SaveKey{2}, k);
        Season s;
        s.year_days = 3.0f;
        s.start_phase = 0.4f;
        se = w.spawn_with(SaveKey{3}, s);
        if (with_leaves) {
            LeafFall l;
            l.enabled = 0;
            le = w.spawn_with(SaveKey{4}, l);
        }
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    std::vector<u32> sample(const LeafFall& lf) const {
        const WorldClock* k = w.try_get<WorldClock>(ce);
        const Season* s = w.try_get<Season>(se);
        const f32 p = season_phase(*s, k->day, k->hour);
        const f32 a = leaf_fall_amount(lf, s, p);
        const LeafFallLook l = leaf_fall_look(lf, a, 2.0f, kEast, season_foliage_tint(*s, p));
        return {k->day,
                std::bit_cast<u32>(p),
                std::bit_cast<u32>(a),
                std::bit_cast<u32>(l.rate),
                std::bit_cast<u32>(l.direction.x),
                std::bit_cast<u32>(l.color_start.x),
                std::bit_cast<u32>(l.color_start.w)};
    }
};

}  // namespace

TEST_CASE("leaf_fall: the component round-trips save and load") {
    Array<byte> bytes;
    u64 hash_a = 0;
    {
        Rig a(true);
        LeafFall& l = *a.w.try_get_mut<LeafFall>(a.le);
        l.enabled = 1;
        l.autumn_start = 0.4f;
        l.autumn_full = 0.5f;
        l.autumn_end_full = 0.7f;
        l.autumn_end = 0.8f;
        l.manual = 0.2f;
        l.rate = 60.0f;
        l.box_xz_m = 20.0f;
        l.height_m = 9.0f;
        l.size_m = 0.2f;
        l.lifetime_s = 9.0f;
        l.fall_speed = 1.2f;
        l.wind_follow = 0.5f;
        l.leaf_color = math::vec3{0.5f, 0.6f, 0.9f};
        l.tint_follow = 0.25f;
        l.max_particles = 2048;
        l.seed = 77;
        l.strength = 0.9f;
        a.run(30);
        save::write_save(a.w, {}, bytes);
        hash_a = a.w.state_hash();
    }
    Rig b(true);
    b.run(5);
    REQUIRE(save::read_save(b.w, bytes.span()));
    const LeafFall& l = *b.w.try_get<LeafFall>(b.le);
    CHECK(l.enabled == 1);
    CHECK(l.autumn_start == 0.4f);
    CHECK(l.autumn_full == 0.5f);
    CHECK(l.autumn_end_full == 0.7f);
    CHECK(l.autumn_end == 0.8f);
    CHECK(l.manual == 0.2f);
    CHECK(l.rate == 60.0f);
    CHECK(l.box_xz_m == 20.0f);
    CHECK(l.height_m == 9.0f);
    CHECK(l.size_m == 0.2f);
    CHECK(l.lifetime_s == 9.0f);
    CHECK(l.fall_speed == 1.2f);
    CHECK(l.wind_follow == 0.5f);
    CHECK(same_vec(l.leaf_color, math::vec3{0.5f, 0.6f, 0.9f}));
    CHECK(l.tint_follow == 0.25f);
    CHECK(l.max_particles == 2048);
    CHECK(l.seed == 77);
    CHECK(l.strength == 0.9f);
    CHECK(l.pad_[0] == 0);
    CHECK(l.pad_[1] == 0);
    CHECK(l.pad_[2] == 0);
    CHECK(b.w.state_hash() == hash_a);
}

TEST_CASE("leaf_fall: the Season, WorldClock and Weather data are the same with and without a LeafFall") {
    Rig a(false), b(true);
    for (u32 i = 0; i < 300; ++i) {
        a.run(1);
        b.run(1);
        const Weather& wa = *a.w.try_get<Weather>(a.we);
        const Weather& wb = *b.w.try_get<Weather>(b.we);
        CHECK(same_bits(wa.wind, wb.wind));
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
    // the leaf code never writes the component: it is still as authored
    const LeafFall& lf = *b.w.try_get<LeafFall>(b.le);
    CHECK(lf.enabled == 0);
    CHECK(lf.autumn_start == 0.5f);
    Rig c(false), d(false);
    c.run(300);
    d.run(300);
    CHECK(c.w.state_hash() == d.w.state_hash());
}

TEST_CASE("leaf_fall: the helpers over a WorldClock run are the same in one go and in two halves") {
    constexpr u32 kN = 1200;
    constexpr u32 kEvery = 20;
    LeafFall authored;
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
