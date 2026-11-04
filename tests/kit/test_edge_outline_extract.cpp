// Extract feed of the edge outline (M-OUTLINE 39d): Kit3DExtract copies the first EdgeOutline3D into Scene3D post.outline and
// runs it through render::apply_quality (tier, r.edge_outline, Range clamps). No component or enabled 0 leaves the disabled
// default (bit-identical to a scene without the feature); nothing is written back to the world.

#include <doctest/doctest.h>

#include <bit>
#include <limits>

#include "mce/convar.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/edge_outline.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
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
    render::EdgeOutline frame() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        return scene.post().outline;
    }
};

ConVar& outline_cvar() {
    ConVar* cv = ConVar::find(StringId("r.edge_outline"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetOutlineCvar {
    ~ResetOutlineCvar() { outline_cvar().reset(); }
};

bool is_default(const render::EdgeOutline& o) {
    const render::EdgeOutline d;
    return o.enabled == d.enabled && same_bits(o.color.x, d.color.x) && same_bits(o.color.y, d.color.y) &&
           same_bits(o.color.z, d.color.z) && same_bits(o.opacity, d.opacity) && same_bits(o.thickness, d.thickness) &&
           same_bits(o.depth_threshold, d.depth_threshold) && same_bits(o.normal_threshold, d.normal_threshold) &&
           same_bits(o.depth_weight, d.depth_weight) && same_bits(o.normal_weight, d.normal_weight) &&
           same_bits(o.fade_start, d.fade_start) && same_bits(o.fade_end, d.fade_end) &&
           o.sky_silhouette == d.sky_silhouette && o.fog_follow == d.fog_follow;
}

}  // namespace

TEST_CASE("edge outline extract: no component keeps the disabled default") {
    Rig r;
    for (int i = 0; i < 3; ++i) CHECK(is_default(r.frame()));
    CHECK_FALSE(r.frame().enabled);
}

TEST_CASE("edge outline extract: enabled 0 keeps the disabled default") {
    Rig r;
    EdgeOutline3D c;
    c.enabled = 0;
    c.opacity = 0.5f;
    c.thickness = 3.0f;
    r.world.spawn_with(c);
    CHECK(is_default(r.frame()));
}

TEST_CASE("edge outline extract: an enabled component copies its fields") {
    Rig r;
    EdgeOutline3D c;
    c.color_r = 0.5f;
    c.color_g = 1.5f;
    c.color_b = 8.0f;
    c.opacity = 0.75f;
    c.thickness = 2.5f;
    c.depth_threshold = 0.125f;
    c.normal_threshold = 0.75f;
    c.depth_weight = 0.5f;
    c.normal_weight = 0.25f;
    c.fade_start = 10.0f;
    c.fade_end = 40.0f;
    c.sky_silhouette = 0;
    c.fog_follow = 0;
    r.world.spawn_with(c);
    const render::EdgeOutline o = r.frame();
    CHECK(o.enabled);
    CHECK(same_bits(o.color.x, 0.5f));
    CHECK(same_bits(o.color.y, 1.5f));
    CHECK(same_bits(o.color.z, 8.0f));
    CHECK(same_bits(o.opacity, 0.75f));
    CHECK(same_bits(o.thickness, 2.5f));
    CHECK(same_bits(o.depth_threshold, 0.125f));
    CHECK(same_bits(o.normal_threshold, 0.75f));
    CHECK(same_bits(o.depth_weight, 0.5f));
    CHECK(same_bits(o.normal_weight, 0.25f));
    CHECK(same_bits(o.fade_start, 10.0f));
    CHECK(same_bits(o.fade_end, 40.0f));
    CHECK_FALSE(o.sky_silhouette);
    CHECK_FALSE(o.fog_follow);
}

TEST_CASE("edge outline extract: the default component enables the default setting") {
    Rig r;
    r.world.spawn_with(EdgeOutline3D{});
    render::EdgeOutline expect;
    expect.enabled = true;
    const render::EdgeOutline o = r.frame();
    CHECK(o.enabled);
    CHECK(same_bits(o.color.x, expect.color.x));
    CHECK(same_bits(o.color.z, expect.color.z));
    CHECK(same_bits(o.fade_end, expect.fade_end));
    CHECK(o.sky_silhouette);
    CHECK(o.fog_follow);
}

TEST_CASE("edge outline extract: the first component wins and removing it clears the outline") {
    Rig r;
    EdgeOutline3D a;
    a.thickness = 2.0f;
    const Entity first = r.world.spawn_with(a);
    EdgeOutline3D b;
    b.thickness = 3.0f;
    const Entity second = r.world.spawn_with(b);
    CHECK(same_bits(r.frame().thickness, 2.0f));
    r.world.destroy(first);
    CHECK(same_bits(r.frame().thickness, 3.0f));
    r.world.destroy(second);
    CHECK(is_default(r.frame()));
}

TEST_CASE("edge outline extract: a disabled first component does not fall through to the second") {
    Rig r;
    EdgeOutline3D a;
    a.enabled = 0;
    r.world.spawn_with(a);
    EdgeOutline3D b;
    b.thickness = 3.0f;
    r.world.spawn_with(b);
    CHECK(is_default(r.frame()));
}

TEST_CASE("edge outline extract: every tier keeps the outline enabled") {
    for (render::QualityTier tier : {render::QualityTier::Low, render::QualityTier::Medium, render::QualityTier::High}) {
        INFO(render::quality_tier_name(tier));
        Rig r;
        EdgeOutline3D c;
        c.thickness = 2.0f;
        r.world.spawn_with(c);
        r.extract.set_quality(render::quality_settings(tier));
        const render::EdgeOutline o = r.frame();
        CHECK(o.enabled);  // the 4-tap kernel on Low is the renderer's choice from the tier
        CHECK(same_bits(o.thickness, 2.0f));
    }
}

TEST_CASE("edge outline extract: a tier with the outline off disables it") {
    Rig r;
    r.world.spawn_with(EdgeOutline3D{});
    render::QualitySettings q = render::quality_settings(render::QualityTier::High);
    q.edge_outline = false;
    r.extract.set_quality(q);
    CHECK_FALSE(r.frame().enabled);
}

TEST_CASE("edge outline extract: r.edge_outline 0 disables the outline") {
    ResetOutlineCvar reset;
    Rig r;
    r.world.spawn_with(EdgeOutline3D{});
    CHECK(r.frame().enabled);
    outline_cvar().set_string("0");
    CHECK_FALSE(r.frame().enabled);
    outline_cvar().set_string("1");
    CHECK(r.frame().enabled);
}

TEST_CASE("edge outline extract: out-of-range component values arrive clamped") {
    Rig r;
    EdgeOutline3D c;
    c.color_r = 100.0f;
    c.color_g = -1.0f;
    c.opacity = 9.0f;
    c.thickness = 100.0f;
    c.depth_threshold = 0.0f;
    c.normal_threshold = 100.0f;
    c.depth_weight = -2.0f;
    c.normal_weight = 5.0f;
    c.fade_start = 500.0f;
    c.fade_end = 100.0f;  // below fade_start: ordered
    r.world.spawn_with(c);
    const render::EdgeOutline o = r.frame();
    CHECK(o.enabled);
    CHECK(same_bits(o.color.x, 16.0f));
    CHECK(same_bits(o.color.y, 0.0f));
    CHECK(same_bits(o.opacity, 1.0f));
    CHECK(same_bits(o.thickness, 4.0f));
    CHECK(same_bits(o.depth_threshold, 0.001f));
    CHECK(same_bits(o.normal_threshold, 2.0f));
    CHECK(same_bits(o.depth_weight, 0.0f));
    CHECK(same_bits(o.normal_weight, 1.0f));
    CHECK(same_bits(o.fade_start, 500.0f));
    CHECK(same_bits(o.fade_end, 500.0f));
}

TEST_CASE("edge outline extract: NaN fields fall back to the defaults") {
    Rig r;
    EdgeOutline3D c;
    c.color_r = c.color_g = c.color_b = kNaN;
    c.opacity = c.thickness = c.depth_threshold = c.normal_threshold = kNaN;
    c.depth_weight = c.normal_weight = c.fade_start = c.fade_end = kNaN;
    r.world.spawn_with(c);
    const render::EdgeOutline o = r.frame();
    const render::EdgeOutline d;
    CHECK(o.enabled);
    CHECK(same_bits(o.color.x, d.color.x));
    CHECK(same_bits(o.color.y, d.color.y));
    CHECK(same_bits(o.color.z, d.color.z));
    CHECK(same_bits(o.opacity, d.opacity));
    CHECK(same_bits(o.thickness, d.thickness));
    CHECK(same_bits(o.depth_threshold, d.depth_threshold));
    CHECK(same_bits(o.normal_threshold, d.normal_threshold));
    CHECK(same_bits(o.depth_weight, d.depth_weight));
    CHECK(same_bits(o.normal_weight, d.normal_weight));
    CHECK(same_bits(o.fade_start, d.fade_start));
    CHECK(same_bits(o.fade_end, d.fade_end));
}

TEST_CASE("edge outline extract: the quality step does not leak into the rest of the PostFx") {
    Rig plain;
    plain.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    plain.frame();
    const render::PostFx base = plain.scene.post();
    Rig r;
    r.world.spawn_with(EdgeOutline3D{});
    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    r.frame();
    const render::PostFx with = r.scene.post();
    CHECK(with.bloom == base.bloom);
    CHECK(with.fxaa == base.fxaa);
    CHECK(with.taa == base.taa);
    CHECK(with.dof.enabled == base.dof.enabled);
    CHECK(with.flare.enabled == base.flare.enabled);
}

TEST_CASE("edge outline extract: nothing is written back to the world") {
    Rig r;
    EdgeOutline3D c;
    c.thickness = 2.0f;
    c.fade_end = 500.0f;
    r.world.spawn_with(c);
    const u64 before = r.world.state_hash();
    r.frame();
    r.frame();
    CHECK(r.world.state_hash() == before);
    r.world.query<const EdgeOutline3D&>().each([&](const EdgeOutline3D& e) {
        CHECK(same_bits(e.thickness, 2.0f));
        CHECK(same_bits(e.fade_end, 500.0f));
        CHECK(e.enabled == 1u);
        CHECK(e.pad_[0] == 0u);
    });
}
