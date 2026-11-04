// Extract feed of the sun lens flare (M-LENSFLARE 38d): Kit3DExtract copies the first LensFlare3D into Scene3D post.flare with
// `visibility` from lens_flare_visibility (post-quality cloud cover and the first Weather's rain). No component or enabled 0
// leaves the disabled default (bit-identical to a scene without the feature); nothing is written back to the world.

#include <doctest/doctest.h>

#include <bit>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/cloud_layer.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/lens_flare.h"
#include "mce/kit/weather.h"
#include "mce/convar.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

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
    render::LensFlare frame() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        return scene.post().flare;
    }
    // A Weather in the given state; the systems are not installed, so it stays as written.
    void weather(WeatherKind kind, f32 intensity) {
        Weather w;
        w.target = kind;
        w.current = kind;
        w.intensity = intensity;
        world.spawn_with(w);
    }
};

ConVar& flare_cvar() {
    ConVar* cv = ConVar::find(StringId("r.lens_flare"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetFlareCvar {
    ~ResetFlareCvar() { flare_cvar().reset(); }
};

bool is_default(const render::LensFlare& f) {
    const render::LensFlare d;
    return f.enabled == d.enabled && same_bits(f.intensity, d.intensity) && same_bits(f.visibility, d.visibility) &&
           same_bits(f.halo, d.halo) && same_bits(f.halo_radius, d.halo_radius) && f.ghost_count == d.ghost_count &&
           same_bits(f.ghost_intensity, d.ghost_intensity) && same_bits(f.ghost_spacing, d.ghost_spacing) &&
           same_bits(f.ghost_chroma, d.ghost_chroma) && same_bits(f.starburst, d.starburst) &&
           f.starburst_rays == d.starburst_rays && same_bits(f.occlusion_radius, d.occlusion_radius) &&
           same_bits(f.edge_fade, d.edge_fade);
}

}  // namespace

TEST_CASE("lens flare extract: no component keeps the disabled default") {
    Rig r;
    for (int i = 0; i < 3; ++i) CHECK(is_default(r.frame()));
    CHECK_FALSE(r.frame().enabled);
}

TEST_CASE("lens flare extract: enabled 0 keeps the disabled default") {
    Rig r;
    LensFlare3D c;
    c.enabled = 0;
    c.intensity = 3.0f;
    c.starburst = 2.0f;
    r.world.spawn_with(c);
    CHECK(is_default(r.frame()));
}

TEST_CASE("lens flare extract: an enabled component copies its fields") {
    Rig r;
    LensFlare3D c;
    c.intensity = 2.5f;
    c.halo = 1.5f;
    c.halo_radius = 0.3f;
    c.ghost_count = 5;
    c.ghost_intensity = 0.75f;
    c.ghost_spacing = 1.25f;
    c.ghost_chroma = 0.25f;
    c.starburst = 1.0f;
    c.starburst_rays = 8;
    c.occlusion_radius = 0.02f;
    c.edge_fade = 0.2f;
    r.world.spawn_with(c);
    const render::LensFlare f = r.frame();
    CHECK(f.enabled);
    CHECK(same_bits(f.intensity, 2.5f));
    CHECK(same_bits(f.visibility, 1.0f));  // clear air
    CHECK(same_bits(f.halo, 1.5f));
    CHECK(same_bits(f.halo_radius, 0.3f));
    CHECK(f.ghost_count == 5u);
    CHECK(same_bits(f.ghost_intensity, 0.75f));
    CHECK(same_bits(f.ghost_spacing, 1.25f));
    CHECK(same_bits(f.ghost_chroma, 0.25f));
    CHECK(same_bits(f.starburst, 1.0f));
    CHECK(f.starburst_rays == 8u);
    CHECK(same_bits(f.occlusion_radius, 0.02f));
    CHECK(same_bits(f.edge_fade, 0.2f));
}

TEST_CASE("lens flare extract: the first component wins and removing it clears the flare") {
    Rig r;
    LensFlare3D a;
    a.intensity = 2.0f;
    const Entity first = r.world.spawn_with(a);
    LensFlare3D b;
    b.intensity = 3.0f;
    const Entity second = r.world.spawn_with(b);
    CHECK(same_bits(r.frame().intensity, 2.0f));
    r.world.destroy(first);
    CHECK(same_bits(r.frame().intensity, 3.0f));
    r.world.destroy(second);
    CHECK(is_default(r.frame()));
}

TEST_CASE("lens flare extract: rain dims the visibility") {
    Rig r;
    r.world.spawn_with(LensFlare3D{});
    r.weather(WeatherKind::Rain, 0.5f);
    CHECK(r.frame().visibility == doctest::Approx(0.5f));
}

TEST_CASE("lens flare extract: a clear or snowy Weather does not dim") {
    Rig r;
    r.world.spawn_with(LensFlare3D{});
    r.weather(WeatherKind::Snow, 0.8f);
    CHECK(same_bits(r.frame().visibility, 1.0f));
}

TEST_CASE("lens flare extract: cloud cover dims the visibility and cloud_dim 0 does not") {
    Rig r;
    LensFlare3D c;
    r.world.spawn_with(c);
    r.weather(WeatherKind::Clear, 0.0f);
    CloudLayer layer;
    layer.fair_coverage = 0.5f;
    layer.strength = 1.0f;
    r.world.spawn_with(layer);
    const f32 v = r.frame().visibility;
    CHECK(v == doctest::Approx(0.5f));
    CHECK(v < 1.0f);

    r.world.query<LensFlare3D&>().each([&](LensFlare3D& f) { f.cloud_dim = 0.0f; });
    CHECK(same_bits(r.frame().visibility, 1.0f));
}

TEST_CASE("lens flare extract: the larger of cloud cover and rain decides") {
    Rig r;
    r.world.spawn_with(LensFlare3D{});
    r.weather(WeatherKind::Rain, 0.25f);
    CloudLayer layer;
    layer.fair_coverage = 0.5f;
    layer.overcast_coverage = 0.5f;  // the same cover whatever the blend
    r.world.spawn_with(layer);
    CHECK(r.frame().visibility == doctest::Approx(0.5f));
}

TEST_CASE("lens flare extract: nothing is written back to the world") {
    Rig r;
    LensFlare3D c;
    c.intensity = 2.0f;
    r.world.spawn_with(c);
    r.frame();
    r.frame();
    r.world.query<const LensFlare3D&>().each([&](const LensFlare3D& f) {
        CHECK(same_bits(f.intensity, 2.0f));
        CHECK(same_bits(f.cloud_dim, 1.0f));
        CHECK(f.enabled == 1u);
    });
}

TEST_CASE("lens flare extract: the Low tier disables the flare") {
    Rig r;
    r.world.spawn_with(LensFlare3D{});
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    CHECK_FALSE(r.frame().enabled);
}

TEST_CASE("lens flare extract: the Medium tier drops the starburst and keeps halo and ghosts") {
    Rig r;
    LensFlare3D c;
    c.starburst = 1.5f;
    c.halo = 1.25f;
    c.ghost_count = 4;
    r.world.spawn_with(c);
    r.extract.set_quality(render::quality_settings(render::QualityTier::Medium));
    const render::LensFlare f = r.frame();
    CHECK(f.enabled);
    CHECK(same_bits(f.starburst, 0.0f));
    CHECK(same_bits(f.halo, 1.25f));
    CHECK(f.ghost_count == 4u);
}

TEST_CASE("lens flare extract: the High tier keeps the starburst") {
    Rig r;
    LensFlare3D c;
    c.starburst = 1.5f;
    r.world.spawn_with(c);
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    const render::LensFlare f = r.frame();
    CHECK(f.enabled);
    CHECK(same_bits(f.starburst, 1.5f));
}

TEST_CASE("lens flare extract: r.lens_flare 0 disables the flare") {
    ResetFlareCvar reset;
    Rig r;
    r.world.spawn_with(LensFlare3D{});
    CHECK(r.frame().enabled);
    flare_cvar().set_string("0");
    CHECK_FALSE(r.frame().enabled);
    flare_cvar().set_string("1");
    CHECK(r.frame().enabled);
}

TEST_CASE("lens flare extract: out-of-range component values arrive clamped") {
    Rig r;
    LensFlare3D c;
    c.ghost_count = 99;
    c.intensity = 100.0f;
    r.world.spawn_with(c);
    const render::LensFlare f = r.frame();
    CHECK(f.enabled);
    CHECK(f.ghost_count == render::kLensFlareMaxGhosts);
    CHECK(same_bits(f.intensity, 4.0f));
}
