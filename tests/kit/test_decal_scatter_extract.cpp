// Extract feed of the decal scatter (M-DECALSCATTER 35b): Kit3DExtract turns each DecalScatter3D into local render::Decals
// (uv_rect from the atlas cell, positions inside the area, opacity faded with the camera distance), nearest to the camera
// first, up to scaled_budget(count) (the Low tier halves it). No component, enabled 0, strength 0 and count 0 add nothing.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/decal_scatter.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"

using namespace mce;
using namespace mce::kit;

namespace {

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    render::Scene3D scene;
    render::Camera3D cam;

    Rig() : scene(store, mem::heap()) {
        install_kit3d(world, nullptr);
        extract.init(store, nullptr);
        world.spawn_with(LocalTransform{}, Camera3DComponent{});
        REQUIRE(extract.camera(world, 1.0f, cam));
    }
    Entity add(const DecalScatter3D& s, math::vec3 p) {
        return world.spawn_with(LocalTransform{p, math::quat::identity(), {1, 1, 1}}, s);
    }
    // The decals before build() (no frustum culling or cap); the camera sits at the origin looking down -Z.
    Array<render::Decal> frame(u32* stat = nullptr) {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        Array<render::Decal> out;
        for (const render::Decal& d : scene.decals()) out.push_back(d);
        if (stat) *stat = extract.stats().decals;
        return out;
    }
};

math::vec3 centre_of(const render::Decal& d) { return {d.world.c[3][0], d.world.c[3][1], d.world.c[3][2]}; }

DecalScatter3D patch(u32 count = 16) {
    DecalScatter3D s;
    s.count = count;
    s.area = {2.0f, 2.0f};
    s.fade_distance_m = 0.0f;
    return s;
}

}  // namespace

TEST_CASE("decal scatter extract: no component adds nothing") {
    Rig r;
    u32 stat = 7;
    const Array<render::Decal> d = r.frame(&stat);
    CHECK(d.size() == 0);
    CHECK(stat == 0);
}

TEST_CASE("decal scatter extract: count 16 gives 16 decals with atlas cells inside the area") {
    Rig r;
    DecalScatter3D s = patch(16);
    s.atlas_cols = 4;
    s.atlas_rows = 2;
    r.add(s, {10.0f, 0.0f, -20.0f});
    u32 stat = 0;
    const Array<render::Decal> d = r.frame(&stat);
    REQUIRE(d.size() == 16);
    CHECK(stat == 16);
    for (const render::Decal& x : d) {
        const math::vec3 c = centre_of(x);
        CHECK(std::abs(c.x - 10.0f) <= 2.0f + 1e-4f);
        CHECK(std::abs(c.z + 20.0f) <= 2.0f + 1e-4f);
        CHECK(c.y == doctest::Approx(0.0f));
        CHECK(x.uv_rect.z == doctest::Approx(0.25f));
        CHECK(x.uv_rect.w == doctest::Approx(0.5f));
        const f32 cx = x.uv_rect.x * 4.0f, cy = x.uv_rect.y * 2.0f;
        CHECK(cx == doctest::Approx(std::round(cx)));
        CHECK(cy == doctest::Approx(std::round(cy)));
        CHECK(cx >= 0.0f);
        CHECK(cx <= 3.0f + 1e-4f);
        CHECK(cy <= 1.0f + 1e-4f);
        CHECK(x.color.w == doctest::Approx(1.0f));
    }
    // Every item is one of the helper's items (the set matches, the order is nearest-first).
    for (u32 i = 0; i < 16; ++i) {
        const ScatterDecal it = decal_scatter_item(s, i);
        bool found = false;
        for (const render::Decal& x : d) {
            const math::vec3 c = centre_of(x);
            if (std::abs(c.x - (10.0f + it.x)) < 1e-3f && std::abs(c.z - (-20.0f + it.z)) < 1e-3f) found = true;
        }
        CHECK(found);
    }
}

TEST_CASE("decal scatter extract: disabled, strength 0 and count 0 add nothing") {
    Rig r;
    DecalScatter3D s = patch();
    s.enabled = 0;
    r.add(s, {0, 0, -5});
    s.enabled = 1;
    s.strength = 0.0f;
    r.add(s, {0, 0, -5});
    s.strength = 1.0f;
    s.count = 0;
    r.add(s, {0, 0, -5});
    u32 stat = 0;
    CHECK(r.frame(&stat).size() == 0);
    CHECK(stat == 0);
}

TEST_CASE("decal scatter extract: a patch beyond the fade distance is skipped, near the limit it is fainter") {
    Rig r;
    DecalScatter3D s = patch(8);
    s.fade_distance_m = 40.0f;
    r.add(s, {0, 0, -100.0f});  // nearest edge at 98 m
    CHECK(r.frame().size() == 0);

    Rig q;
    q.add(s, {0, 0, -3.0f});
    const Array<render::Decal> near_d = q.frame();
    Rig f;
    f.add(s, {0, 0, -37.0f});  // inside the limit: some decals still show, but fainter
    const Array<render::Decal> far_d = f.frame();
    REQUIRE(near_d.size() == 8);
    REQUIRE(far_d.size() > 0);
    f32 near_a = 0.0f, far_a = 0.0f;
    for (const render::Decal& x : near_d) near_a = std::max(near_a, x.color.w);
    for (const render::Decal& x : far_d) far_a = std::max(far_a, x.color.w);
    CHECK(near_a == doctest::Approx(1.0f));
    CHECK(far_a < near_a);
}

TEST_CASE("decal scatter extract: the Low tier keeps half") {
    Rig r;
    r.add(patch(16), {0, 0, -5});
    CHECK(r.frame().size() == 16);
    Rig low;
    low.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    low.add(patch(16), {0, 0, -5});
    const f32 scale = render::quality_settings(render::QualityTier::Low).particle_scale;
    const usize want = std::max<usize>(static_cast<usize>(16.0f * scale), 1);
    CHECK(low.frame().size() == want);
    CHECK(want < 16);
}

TEST_CASE("decal scatter extract: past the 64 cap the nearest decals are the ones kept") {
    Rig r;
    DecalScatter3D s = patch(256);
    s.area = {1.0f, 40.0f};
    r.add(s, {0, 0, -60.0f});
    const Array<render::Decal> d = r.frame();
    REQUIRE(d.size() == 256);
    for (usize i = 1; i < d.size(); ++i) {
        CHECK(length(centre_of(d[i - 1])) <= length(centre_of(d[i])) + 1e-4f);
    }
    f32 far_of_kept = 0.0f, near_of_dropped = 1.0e9f;
    for (usize i = 0; i < d.size(); ++i) {
        const f32 dist = length(centre_of(d[i]));
        if (i < render::kMaxDecals) far_of_kept = std::max(far_of_kept, dist);
        else near_of_dropped = std::min(near_of_dropped, dist);
    }
    CHECK(far_of_kept <= near_of_dropped);
}

TEST_CASE("decal scatter extract: two extracts are equal") {
    Rig a, b;
    DecalScatter3D s = patch(32);
    s.fade_distance_m = 60.0f;
    s.atlas_cols = 3;
    a.add(s, {1, 0, -30});
    b.add(s, {1, 0, -30});
    const Array<render::Decal> x = a.frame();
    const Array<render::Decal> y = b.frame();
    REQUIRE(x.size() == y.size());
    for (usize i = 0; i < x.size(); ++i) {
        for (u32 c = 0; c < 4; ++c)
            for (u32 r = 0; r < 4; ++r) CHECK(x[i].world.c[c][r] == y[i].world.c[c][r]);
        CHECK(x[i].uv_rect == y[i].uv_rect);
        CHECK(x[i].color == y[i].color);
    }
}
