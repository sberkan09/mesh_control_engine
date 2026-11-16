// Caustics3D (M-CAUSTICS 52c): the component defaults match the render setting and the explicit pad is zero;
// Kit3DExtract copies the first enabled one (by entity index) into Scene3D post.caustics through apply_quality (clamps,
// the Low tier, r.caustics 0), and only in a frame whose extract set an enabled water plane (a Water, enabled, with a
// finite level, and r.water on). No component, enabled 0 or no water leaves the disabled default (bit-identical to a scene
// without the feature). The component never touches the water: with and without it the WaterSurface the extract writes
// is the same. Nothing is written back to the world and the state hash over a run does not move.

#include <doctest/doctest.h>

#include <bit>
#include <limits>

#include "mce/convar.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/caustics.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/water.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

constexpr f32 kNaN = std::numeric_limits<f32>::quiet_NaN();

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    render::Scene3D scene;
    render::Camera3D cam;

    explicit Rig(bool water = true) : scene(store, mem::heap()) {
        install_kit3d(world, nullptr);
        extract.init(store, nullptr);
        extract.set_frame_dt(0.25f);
        world.spawn_with(LocalTransform{}, Camera3DComponent{});
        if (water) add_water();
        REQUIRE(extract.camera(world, 1.0f, cam));
    }
    Entity add_water() {
        Water w;
        w.level_y = 1.5f;
        return world.spawn_with(w);
    }
    render::Caustics frame() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        return scene.post().caustics;
    }
};

ConVar& cvar(const char* name) {
    ConVar* cv = ConVar::find(StringId(name));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetCvars {
    ~ResetCvars() {
        cvar("r.caustics").reset();
        cvar("r.water").reset();
    }
};

bool is_default(const render::Caustics& s) {
    const render::Caustics d;
    return s.enabled == d.enabled && same_bits(s.strength, d.strength) && same_bits(s.scale, d.scale) &&
           same_bits(s.speed, d.speed) && same_bits(s.absorption, d.absorption) && same_bits(s.max_depth, d.max_depth) &&
           s.pad_[0] == 0 && s.pad_[1] == 0 && s.pad_[2] == 0;
}

bool same_water(const render::WaterSurface& a, const render::WaterSurface& b) {
    return a.enabled == b.enabled && same_bits(a.level_y, b.level_y) && same_bits(a.half_extent, b.half_extent) &&
           a.wave_count == b.wave_count && same_bits(a.amplitude, b.amplitude) && same_bits(a.wavelength, b.wavelength) &&
           same_bits(a.steepness, b.steepness) && same_bits(a.time, b.time) && same_bits(a.speed_scale, b.speed_scale) &&
           same_bits(a.shallow_color.x, b.shallow_color.x) && same_bits(a.deep_color.z, b.deep_color.z) &&
           same_bits(a.depth_scale_m, b.depth_scale_m) && same_bits(a.reflectivity, b.reflectivity) &&
           same_bits(a.foam_amount, b.foam_amount);
}

}  // namespace

TEST_CASE("caustics: component defaults match the render setting and the pad is zero") {
    const Caustics3D c;
    const render::Caustics r;
    CHECK(c.enabled == 1u);
    CHECK(same_bits(c.strength, r.strength));
    CHECK(same_bits(c.scale, r.scale));
    CHECK(same_bits(c.speed, r.speed));
    CHECK(same_bits(c.absorption, r.absorption));
    CHECK(same_bits(c.max_depth, r.max_depth));
    CHECK(same_bits(c.strength, 1.0f));
    CHECK(same_bits(c.scale, 2.0f));
    CHECK(same_bits(c.speed, 1.0f));
    CHECK(same_bits(c.absorption, 0.15f));
    CHECK(same_bits(c.max_depth, 30.0f));
    CHECK(c.pad_[0] == 0u);
    CHECK(c.pad_[1] == 0u);
    CHECK(c.pad_[2] == 0u);
}

TEST_CASE("caustics extract: no component keeps the disabled default") {
    Rig r;
    for (int i = 0; i < 3; ++i) CHECK(is_default(r.frame()));
}

TEST_CASE("caustics extract: enabled 0 keeps the disabled default") {
    Rig r;
    Caustics3D c;
    c.enabled = 0;
    c.strength = 3.0f;
    r.world.spawn_with(c);
    CHECK(is_default(r.frame()));
}

TEST_CASE("caustics extract: an enabled component reaches PostFx::caustics unchanged") {
    Rig r;
    Caustics3D c;
    c.strength = 2.5f;
    c.scale = 3.0f;
    c.speed = 0.5f;
    c.absorption = 0.4f;
    c.max_depth = 12.0f;
    r.world.spawn_with(c);
    const render::Caustics s = r.frame();
    CHECK(s.enabled);
    CHECK(same_bits(s.strength, 2.5f));
    CHECK(same_bits(s.scale, 3.0f));
    CHECK(same_bits(s.speed, 0.5f));
    CHECK(same_bits(s.absorption, 0.4f));
    CHECK(same_bits(s.max_depth, 12.0f));
    CHECK(s.pad_[0] == 0u);
}

TEST_CASE("caustics extract: out-of-range values arrive clamped, NaN and infinities as the clamp or the default") {
    Rig r;
    Caustics3D c;
    c.strength = 9.0f;
    c.scale = 0.01f;
    c.speed = 7.0f;
    c.absorption = -1.0f;
    c.max_depth = 1000.0f;
    r.world.spawn_with(c);
    render::Caustics s = r.frame();
    CHECK(s.enabled);
    CHECK(same_bits(s.strength, 4.0f));
    CHECK(same_bits(s.scale, 0.25f));
    CHECK(same_bits(s.speed, 4.0f));
    CHECK(same_bits(s.absorption, 0.0f));
    CHECK(same_bits(s.max_depth, 200.0f));
    r.world.query<Caustics3D&>().each([](Caustics3D& v) {
        v.strength = -2.0f;
        v.scale = 100.0f;
        v.speed = -1.0f;
        v.absorption = 50.0f;
        v.max_depth = 0.0f;
    });
    s = r.frame();
    CHECK(same_bits(s.strength, 0.0f));
    CHECK(same_bits(s.scale, 16.0f));
    CHECK(same_bits(s.speed, 0.0f));
    CHECK(same_bits(s.absorption, 4.0f));
    CHECK(same_bits(s.max_depth, 0.5f));
    r.world.query<Caustics3D&>().each([](Caustics3D& v) {
        v.strength = kNaN;
        v.scale = kNaN;
        v.speed = kNaN;
        v.absorption = kNaN;
        v.max_depth = kNaN;
    });
    s = r.frame();
    CHECK(s.enabled);
    CHECK(same_bits(s.strength, 1.0f));
    CHECK(same_bits(s.scale, 2.0f));
    CHECK(same_bits(s.speed, 1.0f));
    CHECK(same_bits(s.absorption, 0.15f));
    CHECK(same_bits(s.max_depth, 30.0f));
    r.world.query<Caustics3D&>().each([](Caustics3D& v) {
        v.strength = std::numeric_limits<f32>::infinity();
        v.max_depth = -std::numeric_limits<f32>::infinity();
    });
    s = r.frame();
    CHECK(same_bits(s.strength, 4.0f));
    CHECK(same_bits(s.max_depth, 0.5f));
}

TEST_CASE("caustics extract: the first enabled component by entity index wins; removing it clears the effect") {
    Rig r;
    Caustics3D off;
    off.enabled = 0;
    off.strength = 3.0f;
    const Entity e0 = r.world.spawn_with(off);
    Caustics3D a;
    a.strength = 0.2f;
    const Entity e1 = r.world.spawn_with(a);
    Caustics3D b;
    b.strength = 0.4f;
    const Entity e2 = r.world.spawn_with(b, LocalTransform{});  // another table: the pick is by index, not table order
    REQUIRE(e0.index < e1.index);
    REQUIRE(e1.index < e2.index);
    CHECK(same_bits(r.frame().strength, 0.2f));
    r.world.destroy(e1);
    CHECK(same_bits(r.frame().strength, 0.4f));
    r.world.destroy(e2);
    CHECK(is_default(r.frame()));
}

TEST_CASE("caustics extract: no water plane in the frame keeps the disabled default") {
    SUBCASE("no Water at all") {
        Rig r(false);
        r.world.spawn_with(Caustics3D{});
        CHECK(is_default(r.frame()));
        // a Water added later turns it on; removing it again turns it off although Scene3D keeps the stale plane
        const Entity w = r.add_water();
        CHECK(r.frame().enabled);
        r.world.destroy(w);
        CHECK(is_default(r.frame()));
        CHECK_FALSE(r.scene.water().enabled);  // the extract clears the carrier when no enabled Water drives it
    }
    SUBCASE("a disabled Water") {
        Rig r(false);
        Water w;
        w.enabled = 0;
        r.world.spawn_with(w);
        r.world.spawn_with(Caustics3D{});
        CHECK(is_default(r.frame()));
    }
    SUBCASE("a non-finite water level") {
        Rig r(false);
        Water w;
        w.level_y = kNaN;
        r.world.spawn_with(w);
        r.world.spawn_with(Caustics3D{});
        CHECK(is_default(r.frame()));
    }
    SUBCASE("r.water 0") {
        ResetCvars reset;
        Rig r;
        r.world.spawn_with(Caustics3D{});
        CHECK(r.frame().enabled);
        cvar("r.water").set_string("0");
        CHECK(is_default(r.frame()));
        cvar("r.water").set_string("1");
        CHECK(r.frame().enabled);
    }
}

TEST_CASE("caustics extract: the Low tier and r.caustics 0 turn it off; Medium and High keep it") {
    ResetCvars reset;
    Rig r;
    r.world.spawn_with(Caustics3D{});
    CHECK(r.frame().enabled);
    cvar("r.caustics").set_string("0");
    CHECK_FALSE(r.frame().enabled);
    cvar("r.caustics").set_string("1");
    CHECK(r.frame().enabled);
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    CHECK_FALSE(r.frame().enabled);
    r.extract.set_quality(render::quality_settings(render::QualityTier::Medium));
    CHECK(r.frame().enabled);
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    CHECK(r.frame().enabled);
}

TEST_CASE("caustics extract: the component does not change the water plane") {
    auto water = [](bool with_component) {
        Rig r;
        if (with_component) {
            Caustics3D c;
            c.strength = 3.0f;
            r.world.spawn_with(c);
        }
        r.frame();
        return r.scene.water();
    };
    const render::WaterSurface a = water(false);
    const render::WaterSurface b = water(true);
    CHECK(a.enabled);
    CHECK(same_water(a, b));
}

TEST_CASE("caustics extract: nothing else in PostFx changes and nothing is written back; the state hash does not move") {
    auto run = [](bool with_component) {
        Rig r;
        if (with_component) {
            Caustics3D c;
            c.strength = 2.0f;
            r.world.spawn_with(c);
        }
        r.frame();
        const u64 before = r.world.state_hash();
        for (int i = 0; i < 40; ++i) r.frame();
        CHECK(r.world.state_hash() == before);
        if (with_component)
            r.world.query<const Caustics3D&>().each([](const Caustics3D& c) {
                CHECK(same_bits(c.strength, 2.0f));
                CHECK(c.enabled == 1u);
            });
        return r.scene.post();
    };
    const render::PostFx a = run(true);
    const render::PostFx b = run(false);
    CHECK(a.caustics.enabled);
    CHECK_FALSE(b.caustics.enabled);
    CHECK(is_default(b.caustics));
    CHECK(a.cloud_shadow.enabled == b.cloud_shadow.enabled);
    CHECK(same_bits(a.shafts.intensity, b.shafts.intensity));
    CHECK(same_bits(a.bloom_intensity, b.bloom_intensity));
    CHECK(a.underwater.enabled == b.underwater.enabled);
    CHECK(a.flare.enabled == b.flare.enabled);
}
