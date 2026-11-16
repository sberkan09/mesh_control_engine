// Extract feed of the heat haze (M-HEATHAZE 43d): Kit3DExtract fills Scene3D post.haze from the HeatHaze3D entities' world
// poses (the first eight in entity id order), through apply_quality (tier cap, r.heat_haze, the Range clamps). No component
// or enabled 0 leaves the empty default; nothing is written back to the world. The centres are in the same render-relative
// space as the lights and the meshes, also after a floating-origin rebase.

#include <doctest/doctest.h>

#include <bit>

#include "mce/convar.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/origin.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/heat_haze.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/light_flicker.h"
#include "mce/render/quality.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }
bool same_vec(math::vec3 a, math::vec3 b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }

const asset::Guid kBoxMesh{0x43d0000000000042ull, 0x8000000000000043ull};

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
        mesh_fixture::Builder box;
        box.add_node(-1);
        mesh_fixture::Box b;
        b.half = {0.5f, 0.5f, 0.5f};
        box.add_box(b);
        const Array<byte> blob = box.build();
        REQUIRE(store.upload(1, 1, blob.span()).has_value());
        extract.bind_mesh(kBoxMesh, 1);
        world.spawn_with(LocalTransform{}, Camera3DComponent{});
        REQUIRE(extract.camera(world, 1.0f, cam));
    }
    render::HeatHaze frame() {
        REQUIRE(extract.camera(world, 1.0f, cam));
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        return scene.post().haze;
    }
    Entity haze(math::vec3 p, HeatHaze3D c = {}) {
        return world.spawn_with(LocalTransform{p, math::quat::identity(), {1, 1, 1}}, c);
    }
};

ConVar& haze_cvar() {
    ConVar* cv = ConVar::find(StringId("r.heat_haze"));
    REQUIRE(cv != nullptr);
    return *cv;
}

struct ResetHazeCvar {
    ~ResetHazeCvar() { haze_cvar().reset(); }
};

bool is_empty(const render::HeatHaze& h) {
    const render::HeatHaze d;
    if (h.count != d.count) return false;
    for (u32 i = 0; i < render::kHeatHazeMaxVolumes; ++i)
        if (!same_vec(h.volumes[i].center, d.volumes[i].center) || !same_bits(h.volumes[i].radius, d.volumes[i].radius) ||
            !same_bits(h.volumes[i].strength, d.volumes[i].strength))
            return false;
    return true;
}

}  // namespace

TEST_CASE("heat haze extract: no component keeps the empty default") {
    Rig r;
    for (int i = 0; i < 3; ++i) CHECK(is_empty(r.frame()));
}

TEST_CASE("heat haze extract: enabled 0 keeps the empty default") {
    Rig r;
    HeatHaze3D c;
    c.enabled = 0;
    r.haze({1, 2, 3}, c);
    CHECK(is_empty(r.frame()));
}

TEST_CASE("heat haze extract: an enabled component copies its fields, the centre is the pose plus offset_y") {
    Rig r;
    HeatHaze3D c;
    c.radius = 2.5f;
    c.strength = 0.75f;
    c.rise = 1.5f;
    c.scale = 2.0f;
    c.softness = 0.25f;
    c.offset_y = 1.5f;
    r.haze({1.0f, 2.0f, -6.0f}, c);
    const render::HeatHaze h = r.frame();
    REQUIRE(h.count == 1u);
    CHECK(same_vec(h.volumes[0].center, math::vec3{1.0f, 3.5f, -6.0f}));
    CHECK(same_bits(h.volumes[0].radius, 2.5f));
    CHECK(same_bits(h.volumes[0].strength, 0.75f));
    CHECK(same_bits(h.volumes[0].rise, 1.5f));
    CHECK(same_bits(h.volumes[0].scale, 2.0f));
    CHECK(same_bits(h.volumes[0].softness, 0.25f));
}

TEST_CASE("heat haze extract: a WorldTransform wins over the LocalTransform") {
    Rig r;
    r.world.register_components<WorldTransform>();
    WorldTransform w;
    w.position = {7.0f, 1.0f, -9.0f};
    const Entity e = r.haze({100.0f, 100.0f, 100.0f});
    r.world.add(e, w);
    const render::HeatHaze h = r.frame();
    REQUIRE(h.count == 1u);
    CHECK(same_vec(h.volumes[0].center, math::vec3{7.0f, 1.0f, -9.0f}));
}

TEST_CASE("heat haze extract: the Range clamps apply") {
    Rig r;
    HeatHaze3D c;
    c.radius = 500.0f;
    c.strength = 9.0f;
    c.rise = 99.0f;
    c.scale = 0.0f;
    c.softness = -1.0f;
    r.haze({0, 0, -5}, c);
    const render::HeatHaze h = r.frame();
    REQUIRE(h.count == 1u);
    CHECK(same_bits(h.volumes[0].radius, 50.0f));
    CHECK(same_bits(h.volumes[0].strength, 2.0f));
    CHECK(same_bits(h.volumes[0].rise, 4.0f));
    CHECK(same_bits(h.volumes[0].scale, 0.25f));
    CHECK(same_bits(h.volumes[0].softness, 0.0f));
}

TEST_CASE("heat haze extract: the first eight in entity id order, whatever the table order") {
    Rig r;
    // Entities 0..11 alternate between two archetypes (with and without a PointLight3D), so the query visits them out of
    // id order; the picked volumes must still be the eight lowest ids.
    Entity made[12];
    for (int i = 0; i < 12; ++i) {
        HeatHaze3D c;
        c.radius = 1.0f + f32(i);
        made[i] = r.haze({f32(i), 0.0f, -5.0f}, c);
        if (i % 2 == 0) r.world.add(made[i], PointLight3D{});
    }
    render::HeatHaze h;
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    h = r.frame();
    REQUIRE(h.count == 8u);
    u32 lowest[8];
    {
        // The eight lowest entity indices of the twelve, ascending.
        u32 idx[12];
        for (int i = 0; i < 12; ++i) idx[i] = made[i].index;
        for (int a = 0; a < 12; ++a)
            for (int b = a + 1; b < 12; ++b)
                if (idx[b] < idx[a]) std::swap(idx[a], idx[b]);
        for (int i = 0; i < 8; ++i) lowest[i] = idx[i];
    }
    for (u32 i = 0; i < 8; ++i) {
        // volume i belongs to the entity with index lowest[i]: find its radius.
        f32 want = -1.0f;
        for (int k = 0; k < 12; ++k)
            if (made[k].index == lowest[i]) want = 1.0f + f32(k);
        CHECK(same_bits(h.volumes[i].radius, want));
    }
    // Medium keeps the first four of those.
    r.extract.set_quality(render::quality_settings(render::QualityTier::Medium));
    const render::HeatHaze m = r.frame();
    REQUIRE(m.count == 4u);
    for (u32 i = 0; i < 4; ++i) CHECK(same_bits(m.volumes[i].radius, h.volumes[i].radius));
}

TEST_CASE("heat haze extract: removing the entities clears the haze") {
    Rig r;
    const Entity a = r.haze({0, 0, -5});
    CHECK(r.frame().count == 1u);
    r.world.destroy(a);
    CHECK(is_empty(r.frame()));
}

TEST_CASE("heat haze extract: nothing is written back to the world") {
    Rig r;
    HeatHaze3D c;
    c.strength = 5.0f;  // out of range: the scene gets 2, the component keeps 5
    c.fire_follow = 1.0f;
    const Entity e = r.haze({0, 0, -5}, c);
    r.frame();
    r.frame();
    const HeatHaze3D* got = r.world.try_get<HeatHaze3D>(e);
    REQUIRE(got != nullptr);
    CHECK(same_bits(got->strength, 5.0f));
    CHECK(same_bits(got->fire_follow, 1.0f));
}

TEST_CASE("heat haze extract: fire_follow scales the strength by the entity's point light") {
    Rig r;
    HeatHaze3D c;
    c.strength = 0.5f;
    c.fire_follow = 1.0f;
    PointLight3D bright;
    bright.intensity = 6.0f;
    bright.radius = 7.0f;  // twice the reference brightness: x2
    const Entity e = r.haze({0, 0, -5}, c);
    r.world.add(e, bright);
    CHECK(same_bits(r.frame().volumes[0].strength, 0.5f * heat_haze_fire_scale(1.0f, 6.0f, 7.0f)));
    // No light on the entity: unchanged.
    const Entity plain = r.haze({3, 0, -5}, c);
    (void)plain;
    // fire_follow 0 ignores the light.
    r.world.try_get_mut<HeatHaze3D>(e)->fire_follow = 0.0f;
    CHECK(same_bits(r.frame().volumes[0].strength, 0.5f));
}

TEST_CASE("heat haze extract: fire_follow includes the light's flicker at the frame clock") {
    Rig r;
    HeatHaze3D c;
    c.fire_follow = 1.0f;
    PointLight3D l;
    l.intensity = 3.0f;
    l.radius = 7.0f;
    LightFlicker3D f;
    f.style = 0;
    f.depth = 0.8f;
    f.radius_depth = 0.4f;
    f.rate_hz = 8.0f;
    const Entity e = r.haze({0, 0, -5}, c);
    r.world.add(e, l);
    r.world.add(e, f);
    bool varied = false;
    f32 first = 0.0f;
    for (int i = 0; i < 12; ++i) {
        const f64 t = static_cast<f64>(r.extract.time() + 0.25f);
        const f32 fi = 3.0f * light_flicker_scale(f, t, e.index);
        const f32 fr = 7.0f * light_flicker_radius_scale(f, t, e.index);
        const f32 got = r.frame().volumes[0].strength;
        CHECK(same_bits(got, 0.5f * heat_haze_fire_scale(1.0f, fi, fr)));
        if (i == 0) first = got;
        else if (!same_bits(got, first)) varied = true;
    }
    CHECK(varied);
}

TEST_CASE("heat haze extract: the Low tier and r.heat_haze 0 disable it, the tier caps the count") {
    ResetHazeCvar reset;
    Rig r;
    r.haze({0, 0, -5});
    r.extract.set_quality(render::quality_settings(render::QualityTier::Medium));
    CHECK(r.frame().count == 1u);

    haze_cvar().set_string("0");
    CHECK(r.frame().count == 0u);
    haze_cvar().reset();
    CHECK(r.frame().count == 1u);

    r.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    CHECK(r.frame().count == 0u);
    r.extract.set_quality(render::quality_settings(render::QualityTier::High));
    CHECK(r.frame().count == 1u);
}

// Floating origin (M-OPENWORLD A2): Forward3D rebases its world-space state by Scene3D::origin_shift, and the lights and
// meshes come from the world that rebase_world already moved. The haze centres must be in that same space, or a volume
// would sit one cell (256 m) away from its fire after a rebase.
TEST_CASE("heat haze extract: centres follow a floating-origin rebase with the lights and the meshes") {
    Rig r;
    install_world_origin(r.world, 256.0f);
    const math::vec3 at{4.0f, 1.0f, -30.0f};
    HeatHaze3D c;
    c.offset_y = 0.0f;
    const Entity fire = r.haze(at, c);
    PointLight3D l;
    l.radius = 400.0f;  // never culled
    r.world.add(fire, l);
    MeshRef m;
    m.mesh.guid = kBoxMesh;
    m.cull_radius = 1000.0f;
    r.world.add(fire, m);

    auto sample = [&](math::vec3& haze_c, math::vec3& light_p, math::vec3& mesh_p) {
        const render::HeatHaze h = r.frame();
        REQUIRE(h.count == 1u);
        REQUIRE(r.scene.point_lights().size() == 1u);
        REQUIRE(r.scene.instances().size() == 1u);
        haze_c = h.volumes[0].center;
        light_p = r.scene.point_lights()[0].position;
        const math::mat4& w = r.scene.instances()[0].world;
        mesh_p = {w.c[3][0], w.c[3][1], w.c[3][2]};
    };

    math::vec3 hc, lp, mp;
    sample(hc, lp, mp);
    CHECK(same_vec(hc, at));
    CHECK(same_vec(lp, at));
    CHECK(same_vec(mp, at));

    // Rebase one cell along +x and -z: every root pose moves by -cells * snap.
    const i32 cells[3] = {1, 0, -1};
    rebase_world(r.world, cells);
    const math::vec3 moved{at.x - 256.0f, at.y, at.z + 256.0f};
    sample(hc, lp, mp);
    CHECK(same_vec(lp, moved));
    CHECK(same_vec(hc, moved));
    CHECK(same_vec(mp, moved));
    CHECK(same_vec(hc, lp));
    CHECK(same_vec(hc, mp));
    // The frame after the rebase tells Forward3D (the camera cut and its own origin move).
    CHECK(same_vec(r.scene.origin_shift(), math::vec3{256.0f, 0.0f, -256.0f}));
    // The absolute position (render-relative plus the origin) is unchanged.
    const WorldOrigin* o = world_origin(r.world);
    REQUIRE(o != nullptr);
    CHECK(same_bits(hc.x + f32(o->cell[0]) * o->snap, at.x));
    CHECK(same_bits(hc.z + f32(o->cell[2]) * o->snap, at.z));
}
