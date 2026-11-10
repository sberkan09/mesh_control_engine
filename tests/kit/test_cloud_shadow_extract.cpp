// CloudShadow3D (M-CLOUDSHADOW 51d): the component defaults match the render setting and the explicit pad is zero;
// Kit3DExtract copies the first enabled one (by entity index) into Scene3D post.cloud_shadow through apply_quality (clamps,
// the Low tier, r.cloud_shadows 0). No component or enabled 0 leaves the disabled default (bit-identical to a scene without
// the feature). The component never touches the sky layer: with and without it, with and without a CloudLayer, the Clouds
// the extract writes are the same; the pass itself needs coverage above 0, so no CloudLayer is no shadow. Nothing is
// written back to the world and the state hash over a run does not move.

#include <doctest/doctest.h>

#include <bit>
#include <limits>

#include "mce/convar.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/cloud_shadow.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/weather.h"
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

    Rig() : scene(store, mem::heap()) {
        install_kit3d(world, nullptr);
        extract.init(store, nullptr);
        extract.set_frame_dt(0.25f);
        world.spawn_with(LocalTransform{}, Camera3DComponent{});
        REQUIRE(extract.camera(world, 1.0f, cam));
    }
    render::CloudShadow frame() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        return scene.post().cloud_shadow;
    }
    void add_clouds(f32 coverage) {
        Weather w;
        w.target = w.current = WeatherKind::Clear;
        world.spawn_with(w);
        CloudLayer layer;
        layer.fair_coverage = coverage;
        layer.overcast_coverage = coverage;
        layer.strength = 1.0f;
        world.spawn_with(layer);
    }
};

ConVar& cs_cvar() {
    ConVar* cv = ConVar::find(StringId("r.cloud_shadows"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetCvar {
    ~ResetCvar() { cs_cvar().reset(); }
};

bool is_default(const render::CloudShadow& s) {
    const render::CloudShadow d;
    return s.enabled == d.enabled && same_bits(s.strength, d.strength) && same_bits(s.softness, d.softness) &&
           s.pad_[0] == 0 && s.pad_[1] == 0 && s.pad_[2] == 0;
}

bool same_clouds(const render::Clouds& a, const render::Clouds& b) {
    return same_bits(a.coverage, b.coverage) && same_bits(a.density, b.density) && same_bits(a.scale_m, b.scale_m) &&
           same_bits(a.speed, b.speed) && same_bits(a.wind_dir.x, b.wind_dir.x) && same_bits(a.wind_dir.y, b.wind_dir.y) &&
           same_bits(a.height_m, b.height_m) && a.enabled == b.enabled;
}

}  // namespace

TEST_CASE("cloud shadow: component defaults match the render setting and the pad is zero") {
    const CloudShadow3D c;
    const render::CloudShadow r;
    CHECK(c.enabled == 1u);
    CHECK(same_bits(c.strength, r.strength));
    CHECK(same_bits(c.softness, r.softness));
    CHECK(same_bits(c.strength, 0.6f));
    CHECK(same_bits(c.softness, 0.3f));
    CHECK(c.pad_[0] == 0u);
    CHECK(c.pad_[1] == 0u);
    CHECK(c.pad_[2] == 0u);
}

TEST_CASE("cloud shadow extract: no component keeps the disabled default") {
    Rig r;
    for (int i = 0; i < 3; ++i) CHECK(is_default(r.frame()));
}

TEST_CASE("cloud shadow extract: enabled 0 keeps the disabled default") {
    Rig r;
    CloudShadow3D c;
    c.enabled = 0;
    c.strength = 1.0f;
    r.world.spawn_with(c);
    CHECK(is_default(r.frame()));
}

TEST_CASE("cloud shadow extract: an enabled component reaches PostFx::cloud_shadow unchanged") {
    Rig r;
    CloudShadow3D c;
    c.strength = 0.8f;
    c.softness = 0.1f;
    r.world.spawn_with(c);
    const render::CloudShadow s = r.frame();
    CHECK(s.enabled);
    CHECK(same_bits(s.strength, 0.8f));
    CHECK(same_bits(s.softness, 0.1f));
}

TEST_CASE("cloud shadow extract: out-of-range values arrive clamped, NaN as the default") {
    Rig r;
    CloudShadow3D c;
    c.strength = 5.0f;
    c.softness = -1.0f;
    const Entity e = r.world.spawn_with(c);
    render::CloudShadow s = r.frame();
    CHECK(s.enabled);
    CHECK(same_bits(s.strength, 1.0f));
    CHECK(same_bits(s.softness, 0.0f));
    r.world.query<CloudShadow3D&>().each([](CloudShadow3D& v) {
        v.strength = -2.0f;
        v.softness = 9.0f;
    });
    s = r.frame();
    CHECK(same_bits(s.strength, 0.0f));
    CHECK(same_bits(s.softness, 1.0f));
    r.world.query<CloudShadow3D&>().each([](CloudShadow3D& v) {
        v.strength = kNaN;
        v.softness = kNaN;
    });
    s = r.frame();
    CHECK(same_bits(s.strength, 0.6f));
    CHECK(same_bits(s.softness, 0.3f));
    (void)e;
}

TEST_CASE("cloud shadow extract: the first enabled component by entity index wins; removing it clears the effect") {
    Rig r;
    CloudShadow3D off;
    off.enabled = 0;
    off.strength = 1.0f;
    const Entity e0 = r.world.spawn_with(off);
    CloudShadow3D a;
    a.strength = 0.2f;
    const Entity e1 = r.world.spawn_with(a);
    CloudShadow3D b;
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

TEST_CASE("cloud shadow extract: the Low tier and r.cloud_shadows 0 turn it off") {
    ResetCvar reset;
    Rig r;
    r.world.spawn_with(CloudShadow3D{});
    CHECK(r.frame().enabled);
    cs_cvar().set_string("0");
    CHECK_FALSE(r.frame().enabled);
    cs_cvar().set_string("1");
    CHECK(r.frame().enabled);
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    CHECK_FALSE(r.frame().enabled);
    r.extract.set_quality(render::quality_settings(render::QualityTier::Medium));
    CHECK(r.frame().enabled);
}

TEST_CASE("cloud shadow extract: the component does not change the sky layer, with or without a CloudLayer") {
    // The sky's clouds come from CloudLayer and the weather only. No CloudLayer: coverage 0 (the pass records nothing even
    // with the component enabled); with one: the Clouds are the same bit for bit with and without the component.
    auto clouds = [](bool with_layer, bool with_component) {
        Rig r;
        if (with_layer) r.add_clouds(0.6f);
        if (with_component) r.world.spawn_with(CloudShadow3D{});
        r.frame();
        return r.scene.clouds();
    };
    const render::Clouds none_a = clouds(false, false);
    const render::Clouds none_b = clouds(false, true);
    CHECK(same_clouds(none_a, none_b));
    CHECK(none_b.coverage == 0.0f);  // no CloudLayer = no cloud cover for the pass to shade
    const render::Clouds with_a = clouds(true, false);
    const render::Clouds with_b = clouds(true, true);
    CHECK(same_clouds(with_a, with_b));
    CHECK(with_b.coverage > 0.0f);
}

TEST_CASE("cloud shadow extract: nothing else in PostFx changes and nothing is written back; the state hash does not move") {
    auto run = [](bool with_component) {
        Rig r;
        r.add_clouds(0.6f);
        if (with_component) {
            CloudShadow3D c;
            c.strength = 0.9f;
            r.world.spawn_with(c);
        }
        r.frame();
        const u64 before = r.world.state_hash();
        for (int i = 0; i < 40; ++i) r.frame();
        CHECK(r.world.state_hash() == before);
        if (with_component)
            r.world.query<const CloudShadow3D&>().each([](const CloudShadow3D& c) {
                CHECK(same_bits(c.strength, 0.9f));
                CHECK(c.enabled == 1u);
            });
        return r.scene.post();
    };
    const render::PostFx a = run(true);
    const render::PostFx b = run(false);
    CHECK(a.cloud_shadow.enabled);
    CHECK_FALSE(b.cloud_shadow.enabled);
    CHECK(same_bits(a.shafts.intensity, b.shafts.intensity));
    CHECK(same_bits(a.bloom_intensity, b.bloom_intensity));
    CHECK(a.flare.enabled == b.flare.enabled);
}
