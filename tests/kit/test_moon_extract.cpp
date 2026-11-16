// Moonlight in Kit3DExtract (M-NIGHTSKY 14b): at night the one directional light slot goes to the moon (direction, tint,
// intensity, shadows) when the moon's weighted contribution beats the sun's; sun-up frames, no Moon, enabled 0, no clock
// and a zero level leave the Scene3D light bit for bit as before; Environment3D is never written; the hand-off is
// measured across a full dusk and dawn.

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/moon.h"
#include "mce/kit/weather.h"
#include "mce/kit/world_clock.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_vec(const math::vec3& a, const math::vec3& b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }
bool same_light(const render::DirectionalLight& a, const render::DirectionalLight& b) {
    return same_vec(a.direction, b.direction) && same_vec(a.color, b.color) && same_bits(a.intensity, b.intensity) &&
           same_vec(a.ambient_sky, b.ambient_sky) && same_vec(a.ambient_ground, b.ambient_ground) && a.shadows == b.shadows &&
           a.contact_shadows == b.contact_shadows;
}
using math::dot;

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    render::Scene3D scene;
    render::Camera3D cam;
    Entity moon_e;

    Rig() : scene(store, mem::heap()) {
        mesh_fixture::Builder box;
        box.add_node(-1);
        mesh_fixture::Box b;
        b.half = {0.5f, 0.5f, 0.5f};
        box.add_box(b);
        const Array<byte> blob = box.build();
        REQUIRE(store.upload(1, 1, blob.span()).has_value());
        world.register_components<SaveKey>();
        install_kit3d(world, nullptr);
        install_weather(world, nullptr);
        extract.init(store, nullptr);
        world.spawn_with(LocalTransform{}, Camera3DComponent{});
        world.spawn_with(SaveKey{1}, WorldClock{});
        env().time_of_day = 0.0f;  // midnight
        env().sun_intensity = 0.05f;
        env().sun_color = {0.2f, 0.25f, 0.5f};
        REQUIRE(extract.camera(world, 1.0f, cam));
    }
    void add_moon(const Moon& m = {}) { moon_e = world.spawn_with(SaveKey{2}, m); }
    Moon& moon() { return *world.try_get_mut<Moon>(moon_e); }
    Environment3D& env() { return world.resource<Environment3D>(); }
    render::DirectionalLight frame() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        return scene.light();
    }
};

u32 env_hash(const Environment3D& e) {
    u32 h = 2166136261u;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(&e);
    for (usize i = 0; i < sizeof(Environment3D); ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

}  // namespace

TEST_CASE("moon extract: deep night takes direction, tint, intensity and shadows from the moon") {
    Rig r;
    const render::DirectionalLight base = r.frame();
    Moon m;
    m.light_color = {0.4f, 0.5f, 0.9f};
    m.light_intensity = 0.2f;
    m.light_shadows = 0;
    r.add_moon(m);
    r.env().sun_shadows = 1;
    const u32 before = env_hash(r.env());
    const render::DirectionalLight l = r.frame();
    const f32 level = moon_level(m, 0, 0.0f, nullptr);
    REQUIRE(level > 0.5f);  // a full moon at midnight
    const math::vec3 want = moon_direction(m, 0.0f, moon_phase(m, 0, 0.0f));
    CHECK(same_vec(l.direction, want));
    CHECK(same_vec(l.color, m.light_color));
    CHECK(l.intensity == doctest::Approx(0.2f * level));  // sw is 0 at midnight
    CHECK_FALSE(l.shadows);
    CHECK(base.shadows);
    CHECK(l.direction.y < 0.0f);  // the moon is up: the light travels down
    // the small cool ambient lift
    CHECK(l.ambient_sky.z > base.ambient_sky.z);
    CHECK(l.ambient_sky.z - base.ambient_sky.z == doctest::Approx(0.9f * level * 0.02f));
    CHECK(env_hash(r.env()) == before);  // Environment3D is never written
}

TEST_CASE("moon extract: sun-up frames keep the light bit-identical") {
    Rig n;
    n.env().sun_intensity = 2.0f;
    Rig r;
    r.env().sun_intensity = 2.0f;
    r.add_moon();
    for (f32 h = 7.0f; h <= 17.0f; h += 0.25f) {
        n.env().time_of_day = h;
        r.env().time_of_day = h;
        CHECK(same_light(n.frame(), r.frame()));
    }
}

TEST_CASE("moon extract: a new moon leaves the light on the dim sun") {
    Rig n;
    Rig r;
    Moon m;
    m.freeze_phase = 0.0f;  // new: illumination 0, level exactly 0
    r.add_moon(m);
    for (f32 h = 0.0f; h < 24.0f; h += 0.5f) {
        n.env().time_of_day = h;
        r.env().time_of_day = h;
        CHECK(same_light(n.frame(), r.frame()));
    }
}

TEST_CASE("moon extract: no Moon, enabled 0, no clock and a zero level change nothing") {
    Rig n;
    const render::DirectionalLight base = n.frame();
    Rig r;
    r.add_moon();
    CHECK_FALSE(same_light(base, r.frame()));  // sanity: the moon does change the midnight light
    r.moon().enabled = 0;
    CHECK(same_light(base, r.frame()));
    r.moon().enabled = 1;
    r.env().time_of_day = -1.0f;  // no clock
    n.env().time_of_day = -1.0f;
    CHECK(same_light(n.frame(), r.frame()));
    r.env().time_of_day = 0.0f;
    r.moon().light_intensity = 0.0f;  // level stays > 0, intensity 0: the moon would carry zero light; still the sun is 0.05
    // a zero level: the moon is below the horizon at noon
    r.moon().light_intensity = 0.12f;
    r.env().time_of_day = 12.0f;
    n.env().time_of_day = 12.0f;
    CHECK(same_light(n.frame(), r.frame()));
    // the same with a Weather that damps to nothing at all stays a pure function: a storm lowers the level, not the shape
    r.env().time_of_day = 0.0f;
    const render::DirectionalLight clear = r.frame();
    Weather w;
    w.current = WeatherKind::Storm;
    w.intensity = 1.0f;
    r.world.spawn_with(SaveKey{3}, w);
    const render::DirectionalLight storm = r.frame();
    CHECK(storm.intensity < clear.intensity);
    CHECK(storm.intensity == doctest::Approx(0.12f * 0.15f * moon_level(r.moon(), 0, 0.0f, nullptr)));
}

TEST_CASE("moon extract: the clock's day count sets the phase the light uses") {
    Rig r;
    r.add_moon();
    const f32 full = r.frame().intensity;
    WorldClock* wc = nullptr;
    r.world.query<WorldClock&>().each([&](WorldClock& c) { wc = &c; });
    REQUIRE(wc);
    wc->day = 15;  // about a half cycle later: a new moon, level 0, so the light stays on the dim sun
    const f32 newish = r.frame().intensity;
    CHECK(same_bits(newish, 0.05f));
    CHECK(full > newish);
}

TEST_CASE("moon extract: the weighted rule keeps the sun while it dominates and takes the moon when it does not") {
    Rig r;
    r.env().sun_intensity = 2.0f;
    r.add_moon();
    // sun elevation just under the threshold: sw is tiny but sw * 2 still beats the moon's 0.12 * mw
    bool saw_sun = false, saw_moon = false;
    for (f32 h = 16.0f; h <= 22.0f; h += 0.01f) {
        r.env().time_of_day = h;
        const render::DirectionalLight l = r.frame();
        const math::vec3 sd = render::sun_direction_at(h);
        const f32 sw = [&] {
            const f32 t = std::clamp((-sd.y + 0.05f) / 0.15f, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }();
        const f32 level = moon_level(r.moon(), 0, h, nullptr);
        const f32 mw = (1.0f - sw) * level;
        const bool moon_wins = mw > 0.0f && sw * 2.0f < mw * 0.12f;
        if (moon_wins) {
            saw_moon = true;
            CHECK(same_vec(l.color, r.moon().light_color));
        } else {
            saw_sun = true;
            CHECK(same_vec(l.color, r.env().sun_color));
            CHECK(same_bits(l.intensity, 2.0f));
        }
    }
    CHECK(saw_sun);
    CHECK(saw_moon);
}

namespace {

struct Handoff {
    f32 max_di = 0.0f;     // largest per-tick change of intensity
    f32 max_dang = 0.0f;   // largest per-tick change of direction, degrees
    f32 max_dsum = 0.0f;   // largest per-tick change of the light's irradiance on an up-facing surface (intensity * -dir.y)
    f32 at_h = 0.0f;
    i32 switches = 0;      // sun <-> moon switches
};

// One full day at 60 Hz ticks of a 1200 s day (72000 ticks); `night_key` mimics DayCycle's dim blue night sun.
Handoff sweep(bool night_key, const Moon& m) {
    Rig r;
    r.add_moon(m);
    Handoff out;
    constexpr i32 kTicks = 72000;
    render::DirectionalLight prev;
    bool have_prev = false, prev_moon = false;
    for (i32 i = 0; i < kTicks; ++i) {
        const f32 h = 24.0f * static_cast<f32>(i) / static_cast<f32>(kTicks);
        r.env().time_of_day = h;
        const math::vec3 sd = render::sun_direction_at(h);
        const f32 up = std::clamp((-sd.y + 0.05f) / 0.15f, 0.0f, 1.0f);
        r.env().sun_intensity = night_key ? 0.05f + 1.95f * up * up * (3.0f - 2.0f * up) : 2.0f;
        const render::DirectionalLight l = r.frame();
        const bool is_moon = same_vec(l.color, m.light_color);
        if (have_prev) {
            const f32 di = std::fabs(l.intensity - prev.intensity);
            const f32 c = std::clamp(dot(l.direction, prev.direction) /
                                         std::sqrt(std::max(1e-12f, dot(l.direction, l.direction) * dot(prev.direction, prev.direction))),
                                     -1.0f, 1.0f);
            const f32 ang = std::acos(c) * 57.29578f;
            const f32 dsum = std::fabs(l.intensity * std::max(0.0f, -l.direction.y) - prev.intensity * std::max(0.0f, -prev.direction.y));
            // sun-driven changes (DayCycle ramps) are not the hand-off: count only ticks where the source switches
            if (is_moon != prev_moon) {
                ++out.switches;
                if (di > out.max_di) { out.max_di = di; out.at_h = h; }
                out.max_dang = std::max(out.max_dang, ang);
                out.max_dsum = std::max(out.max_dsum, dsum);
            }
        }
        prev = l;
        prev_moon = is_moon;
        have_prev = true;
    }
    return out;
}

}  // namespace

TEST_CASE("moon extract: hand-off continuity across a full dusk and dawn (measured)") {
    Moon m;  // full moon at day 0, freeze_phase pins it so the whole day has the same phase
    m.freeze_phase = 0.5f;
    for (const bool night_key : {false, true}) {
        const Handoff h = sweep(night_key, m);
        std::printf("[moon handoff] night_key=%d switches=%d max_dI=%.6f max_dAngle=%.3f deg max_dE_up=%.6f at hour %.3f\n",
                    night_key ? 1 : 0, h.switches, static_cast<double>(h.max_di), static_cast<double>(h.max_dang),
                    static_cast<double>(h.max_dsum), static_cast<double>(h.at_h));
        // dusk and dawn: the slot changes hands exactly once each. With a DayCycle-like night key the intensity step is
        // small; a sun that stays at full intensity below the horizon (no DayCycle) is handed over at 2.0 and pops by
        // design (the guide says to pair a Moon with a DayCycle).
        CHECK(h.switches == 2);
        if (night_key) CHECK(h.max_di < 0.1f);
    }
}

TEST_CASE("moon extract: a full moon night is brighter than a new moon night on up-facing ground") {
    auto irradiance = [](f32 phase) {
        Rig r;
        Moon m;
        m.freeze_phase = phase;
        r.add_moon(m);
        r.env().time_of_day = 0.0f;
        const render::DirectionalLight l = r.frame();
        const f32 lit = l.intensity * std::max(0.0f, -l.direction.y);
        return lit + 0.5f * (l.ambient_sky.x + l.ambient_sky.y + l.ambient_sky.z) / 3.0f;
    };
    CHECK(irradiance(0.5f) > irradiance(0.0f));
    CHECK(irradiance(0.5f) > irradiance(0.25f));
    CHECK(irradiance(0.45f) > irradiance(0.0f));
}
