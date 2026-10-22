// GroundMist in Kit3DExtract (M-GROUNDMIST 13b): the mist level raises the local Volumetrics (density, height falloff,
// base height, albedo) when volumetric fog is on and the tier keeps it, and otherwise the exp2 Fog (density, colour),
// exactly one path per frame; Environment3D is never written; no GroundMist, enabled 0 or a zero level leaves the
// Scene3D fog and volumetrics bit for bit as before; weather fog and mist compose additively; a dawn frame has more fog
// than a noon frame (golden-free comparison of the mean fog contribution).

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstring>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/ground_mist.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/weather.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_vec(const math::vec3& a, const math::vec3& b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }

struct Shot {
    render::Fog fog;
    render::Volumetrics vol;
};

bool same(const Shot& a, const Shot& b) {
    return same_vec(a.fog.color, b.fog.color) && same_bits(a.fog.density, b.fog.density) && a.vol.enabled == b.vol.enabled &&
           same_bits(a.vol.density, b.vol.density) && same_bits(a.vol.height_falloff, b.vol.height_falloff) &&
           same_bits(a.vol.base_height, b.vol.base_height) && same_vec(a.vol.albedo, b.vol.albedo) &&
           same_bits(a.vol.max_distance, b.vol.max_distance) && a.vol.tile == b.vol.tile && a.vol.slices == b.vol.slices;
}

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    Entity mist_e;

    Rig() {
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
        Environment3D& env = world.resource<Environment3D>();
        env.time_of_day = 6.0f;  // the peak
        env.fog_density = 0.01f;
        env.fog_color = {0.5f, 0.6f, 0.7f};
        env.volumetric_fog = 1;
        Weather w;  // Clear, wetness 1: humidity 1, so the level at the peak is 1
        w.wetness = 1.0f;
        world.spawn_with(SaveKey{1}, w);
    }
    void add_mist(const GroundMist& m = {}) { mist_e = world.spawn_with(SaveKey{2}, m); }
    GroundMist& mist() { return *world.try_get_mut<GroundMist>(mist_e); }
    Environment3D& env() { return world.resource<Environment3D>(); }
    Shot frame() {
        render::Camera3D cam;
        REQUIRE(extract.camera(world, 1.0f, cam));
        render::Scene3D s(store, mem::heap());
        s.begin(cam, {320, 180});
        extract.extract(world, s, 1.0f);
        return {s.fog(), s.volumetrics()};
    }
};

u32 env_hash(const Environment3D& e) {
    u32 h = 2166136261u;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(&e);
    for (usize i = 0; i < sizeof(Environment3D); ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

}  // namespace

TEST_CASE("ground_mist extract: the volumetric path raises density, falloff, base height and albedo") {
    Rig r;
    const Shot base = r.frame();
    GroundMist m;
    m.density = 0.1f;
    m.thickness_m = 2.0f;  // falloff 0.5 > the environment's 0.15
    m.base_height_m = 3.0f;
    r.add_mist(m);
    const u32 before = env_hash(r.env());
    const Shot f = r.frame();
    CHECK(f.vol.enabled);
    CHECK(f.vol.density == doctest::Approx(base.vol.density + 0.1f));
    CHECK(f.vol.height_falloff == doctest::Approx(0.5f));
    CHECK(f.vol.base_height == doctest::Approx(3.0f));
    CHECK(f.vol.albedo.x < base.vol.albedo.x);  // mixed toward the bluish mist colour
    CHECK(f.vol.albedo.x == doctest::Approx(0.9f + 0.5f * (0.78f - 0.9f)));
    CHECK(same_bits(f.fog.density, base.fog.density));  // exactly one path: the exp2 fog is untouched
    CHECK(same_vec(f.fog.color, base.fog.color));
    CHECK(env_hash(r.env()) == before);  // Environment3D is never written
    CHECK(r.env().volumetric_density == doctest::Approx(0.03f));
}

TEST_CASE("ground_mist extract: a lower level blends the falloff and a thin mist keeps the larger scene falloff") {
    Rig r;
    GroundMist m;
    m.thickness_m = 2.0f;
    m.base_humidity = 0.5f;
    m.wet_gain = 0.0f;  // level 0.5 at the peak
    r.add_mist(m);
    Shot f = r.frame();
    CHECK(f.vol.height_falloff == doctest::Approx(0.15f + 0.5f * (0.5f - 0.15f)));
    CHECK(f.vol.density == doctest::Approx(0.03f + 0.5f * 0.06f));
    r.mist().thickness_m = 200.0f;  // falloff 0.005 < 0.15: the scene's own falloff stays
    f = r.frame();
    CHECK(same_bits(f.vol.height_falloff, 0.15f));
}

TEST_CASE("ground_mist extract: the density is clamped to 1") {
    Rig r;
    r.env().volumetric_density = 0.98f;
    GroundMist m;
    m.density = 1.0f;
    r.add_mist(m);
    CHECK(r.frame().vol.density == 1.0f);
}

TEST_CASE("ground_mist extract: volumetric fog off falls back to the exp2 fog") {
    Rig r;
    r.env().volumetric_fog = 0;
    const Shot base = r.frame();
    GroundMist m;
    m.exp2_density = 0.05f;
    r.add_mist(m);
    const Shot f = r.frame();
    CHECK_FALSE(f.vol.enabled);
    CHECK(f.fog.density == doctest::Approx(base.fog.density + 0.05f));
    CHECK(f.fog.color.x == doctest::Approx(0.78f));  // level 1: the colour is the mist colour
    CHECK(f.fog.color.y == doctest::Approx(0.82f));
    CHECK(f.fog.color.z == doctest::Approx(0.88f));
    // the volumetrics are exactly the unmisted ones
    CHECK(same_bits(f.vol.density, base.vol.density));
    CHECK(same_bits(f.vol.height_falloff, base.vol.height_falloff));
    CHECK(same_bits(f.vol.base_height, base.vol.base_height));
    CHECK(same_vec(f.vol.albedo, base.vol.albedo));
}

TEST_CASE("ground_mist extract: a tier that drops the volumetrics takes the exp2 path, once") {
    Rig r;
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    const Shot base = r.frame();  // volumetric_fog is 1 but Low turns it off
    CHECK_FALSE(base.vol.enabled);
    GroundMist m;
    m.exp2_density = 0.05f;
    m.density = 0.5f;
    r.add_mist(m);
    const Shot f = r.frame();
    CHECK_FALSE(f.vol.enabled);
    CHECK(f.fog.density == doctest::Approx(base.fog.density + 0.05f));  // added once, not twice
    CHECK(same_bits(f.vol.density, base.vol.density));                    // the volumetric path did not apply
    CHECK(same_bits(f.vol.base_height, base.vol.base_height));
    // a second frame from the same state gives the same fog: nothing accumulates
    CHECK(same(f, r.frame()));
    // a High tier with volumetric fog on takes the volumetric path and leaves the exp2 fog alone
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    const Shot hi = r.frame();
    CHECK(hi.vol.enabled);
    CHECK(same_bits(hi.fog.density, base.fog.density));
}

TEST_CASE("ground_mist extract: no component, enabled 0 and level 0 are bit-identical") {
    for (const bool vol_on : {true, false}) {
        Rig r;
        r.env().volumetric_fog = vol_on ? 1 : 0;
        const Shot base = r.frame();
        r.add_mist();
        r.mist().enabled = 0;
        CHECK(same(base, r.frame()));
        r.mist().enabled = 1;
        r.env().time_of_day = 18.0f;  // far from the peak: level 0
        CHECK(same(r.frame(), [&] {
            Rig n;
            n.env().volumetric_fog = vol_on ? 1 : 0;
            n.env().time_of_day = 18.0f;
            return n.frame();
        }()));
        r.env().time_of_day = -1.0f;  // no clock: level 0
        CHECK(same(r.frame(), [&] {
            Rig n;
            n.env().volumetric_fog = vol_on ? 1 : 0;
            n.env().time_of_day = -1.0f;
            return n.frame();
        }()));
        r.env().time_of_day = 6.0f;
        CHECK_FALSE(same(base, r.frame()));  // sanity: at the peak the mist does change the frame
    }
}

TEST_CASE("ground_mist extract: weather fog and mist compose additively") {
    Rig r;
    r.env().volumetric_fog = 0;
    r.env().fog_density = 0.0f;
    const Shot none = r.frame();
    r.env().fog_density = 0.03f;  // what a weather system would have written into the environment
    const Shot weather_only = r.frame();
    GroundMist m;
    m.exp2_density = 0.02f;
    r.add_mist(m);
    const Shot both = r.frame();
    r.env().fog_density = 0.0f;
    const Shot mist_only = r.frame();
    CHECK(both.fog.density == doctest::Approx(weather_only.fog.density + 0.02f));
    CHECK(mist_only.fog.density == doctest::Approx(none.fog.density + 0.02f));
    CHECK(both.fog.density == doctest::Approx(weather_only.fog.density + (mist_only.fog.density - none.fog.density)));
}

TEST_CASE("ground_mist extract: dawn has more fog than noon, on both paths") {
    for (const bool vol_on : {true, false}) {
        Rig r;
        r.env().volumetric_fog = vol_on ? 1 : 0;
        r.add_mist();
        f32 dawn = 0.0f, noon = 0.0f;
        i32 n = 0;
        for (f32 h = 4.0f; h <= 8.0f; h += 0.5f, ++n) {  // mean over the morning
            r.env().time_of_day = h;
            const Shot f = r.frame();
            dawn += vol_on ? f.vol.density : f.fog.density;
        }
        dawn /= static_cast<f32>(n);
        r.env().time_of_day = 12.0f;
        const Shot f = r.frame();
        noon = vol_on ? f.vol.density : f.fog.density;
        const f32 plain = vol_on ? 0.03f : 0.01f;
        CHECK(dawn > noon);
        CHECK(same_bits(noon, plain));  // at noon the mist is gone and the frame is the unmisted one
    }
}
