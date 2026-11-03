// Extract feed of the light flicker (M-FLICKER 36b): Kit3DExtract multiplies a point or spot light's intensity and radius by
// light_flicker_scale / light_flicker_radius_scale at the frame's render clock, salted with the entity index. No component,
// enabled 0 and depth 0 leave the authored numbers bit-identical; culling keeps the authored radius; nothing is written back.

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>

#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/light_flicker.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

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
        extract.set_frame_dt(0.25f);
        world.spawn_with(LocalTransform{}, Camera3DComponent{});
        REQUIRE(extract.camera(world, 1.0f, cam));
    }
    Entity point(math::vec3 p, f32 intensity = 4.0f, f32 radius = 8.0f) {
        PointLight3D l;
        l.intensity = intensity;
        l.radius = radius;
        return world.spawn_with(LocalTransform{p, math::quat::identity(), {1, 1, 1}}, l);
    }
    Entity spot(math::vec3 p, f32 intensity = 6.0f, f32 radius = 12.0f) {
        SpotLight3D l;
        l.intensity = intensity;
        l.radius = radius;
        return world.spawn_with(LocalTransform{p, math::quat::identity(), {1, 1, 1}}, l);
    }
    // The lights of one extract, in query order.
    Array<render::PointLight> frame() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        Array<render::PointLight> out;
        for (const render::PointLight& pl : scene.point_lights()) out.push_back(pl);
        return out;
    }
    // The clock the next (non-bake, first-view) extract renders with.
    f64 next_clock() const { return static_cast<f64>(extract.time() + 0.25f); }
};

LightFlicker3D fire(u32 seed = 1) {
    LightFlicker3D f;
    f.style = 0;
    f.depth = 0.8f;
    f.radius_depth = 0.4f;
    f.rate_hz = 8.0f;
    f.seed = seed;
    return f;
}

bool same_bits(f32 a, f32 b) { return std::bit_cast<u32>(a) == std::bit_cast<u32>(b); }

}  // namespace

TEST_CASE("light flicker extract: no component keeps the authored intensity and radius") {
    Rig r;
    r.point({0, 0, -5}, 4.0f, 8.0f);
    r.spot({0, 0, -5}, 6.0f, 12.0f);
    for (int i = 0; i < 4; ++i) {
        const Array<render::PointLight> l = r.frame();
        REQUIRE(l.size() == 2);
        CHECK(same_bits(l[0].intensity, 4.0f));
        CHECK(same_bits(l[0].radius, 8.0f));
        CHECK(same_bits(l[1].intensity, 6.0f));
        CHECK(same_bits(l[1].radius, 12.0f));
    }
}

TEST_CASE("light flicker extract: a fire light follows the helpers at the frame clock") {
    Rig r;
    const Entity e = r.point({0, 0, -5}, 4.0f, 8.0f);
    r.world.add(e, fire());
    const LightFlicker3D f = fire();
    bool varied = false;
    f32 first = -1.0f;
    for (int i = 0; i < 12; ++i) {
        const f64 t = r.next_clock();
        const Array<render::PointLight> l = r.frame();
        REQUIRE(l.size() == 1);
        CHECK(same_bits(l[0].intensity, 4.0f * light_flicker_scale(f, t, e.index)));
        CHECK(same_bits(l[0].radius, 8.0f * light_flicker_radius_scale(f, t, e.index)));
        CHECK(l[0].intensity <= 4.0f);
        CHECK(l[0].intensity >= 4.0f * (1.0f - 0.8f) - 1e-4f);
        if (first < 0.0f) first = l[0].intensity;
        if (l[0].intensity != first) varied = true;
    }
    CHECK(varied);
}

TEST_CASE("light flicker extract: two lamps with the same seed do not flicker in step") {
    Rig r;
    const Entity a = r.point({-1, 0, -5});
    const Entity b = r.point({1, 0, -5});
    r.world.add(a, fire(7));
    r.world.add(b, fire(7));
    bool differ = false;
    for (int i = 0; i < 12; ++i) {
        const Array<render::PointLight> l = r.frame();
        REQUIRE(l.size() == 2);
        if (l[0].intensity != l[1].intensity) differ = true;
    }
    CHECK(differ);
}

TEST_CASE("light flicker extract: disabled and depth 0 are bit-identical") {
    Rig r;
    const Entity off = r.point({-1, 0, -5}, 4.0f, 8.0f);
    const Entity flat = r.point({1, 0, -5}, 4.0f, 8.0f);
    LightFlicker3D f = fire();
    f.enabled = 0;
    r.world.add(off, f);
    f = fire();
    f.depth = 0.0f;
    f.radius_depth = 0.0f;
    r.world.add(flat, f);
    for (int i = 0; i < 6; ++i) {
        const Array<render::PointLight> l = r.frame();
        REQUIRE(l.size() == 2);
        for (const render::PointLight& pl : l) {
            CHECK(same_bits(pl.intensity, 4.0f));
            CHECK(same_bits(pl.radius, 8.0f));
        }
    }
}

TEST_CASE("light flicker extract: depth 0 with a radius depth still breathes the radius only") {
    Rig r;
    const Entity e = r.point({0, 0, -5}, 4.0f, 8.0f);
    LightFlicker3D f = fire();
    f.depth = 0.0f;
    f.radius_depth = 0.5f;
    r.world.add(e, f);
    bool shrank = false;
    for (int i = 0; i < 12; ++i) {
        const Array<render::PointLight> l = r.frame();
        REQUIRE(l.size() == 1);
        CHECK(same_bits(l[0].intensity, 4.0f));
        CHECK(l[0].radius <= 8.0f);
        if (l[0].radius < 8.0f) shrank = true;
    }
    CHECK(shrank);
}

TEST_CASE("light flicker extract: a spot light flickers too, and the component is not rewritten") {
    Rig r;
    const Entity e = r.spot({0, 0, -5}, 6.0f, 12.0f);
    r.world.add(e, fire(3));
    const LightFlicker3D f = fire(3);
    const f64 t = r.next_clock();
    const Array<render::PointLight> l = r.frame();
    REQUIRE(l.size() == 1);
    CHECK(same_bits(l[0].intensity, 6.0f * light_flicker_scale(f, t, e.index)));
    CHECK(same_bits(l[0].radius, 12.0f * light_flicker_radius_scale(f, t, e.index)));
    CHECK(r.extract.stats().spot_lights == 1);
    const SpotLight3D* s = r.world.try_get<SpotLight3D>(e);
    REQUIRE(s != nullptr);
    CHECK(same_bits(s->intensity, 6.0f));
    CHECK(same_bits(s->radius, 12.0f));
    const LightFlicker3D* fl = r.world.try_get<LightFlicker3D>(e);
    REQUIRE(fl != nullptr);
    CHECK(same_bits(fl->depth, f.depth));
}

TEST_CASE("light flicker extract: bake mode renders the clock as it stands, so frames repeat") {
    Rig r;
    const Entity e = r.point({0, 0, -5}, 4.0f, 8.0f);
    r.world.add(e, fire());
    r.extract.set_bake_mode(true);
    const f64 t = static_cast<f64>(r.extract.time());
    const f32 want = 4.0f * light_flicker_scale(fire(), t, e.index);
    for (int i = 0; i < 3; ++i) {
        const Array<render::PointLight> l = r.frame();
        REQUIRE(l.size() == 1);
        CHECK(same_bits(l[0].intensity, want));
    }
}

TEST_CASE("light flicker extract: culling uses the authored radius") {
    Rig r;
    // Behind the camera: only the authored radius reaches the view; a fully shrunk one (half) would not.
    const Entity e = r.point({0, 0, 3.0f}, 4.0f, 3.2f);
    LightFlicker3D f = fire();
    f.depth = 0.5f;
    f.radius_depth = 0.5f;
    r.world.add(e, f);
    const Array<render::PointLight> with = r.frame();
    Rig plain;
    plain.point({0, 0, 3.0f}, 4.0f, 3.2f);
    const Array<render::PointLight> without = plain.frame();
    CHECK(with.size() == without.size());
    for (const render::PointLight& pl : with) CHECK(pl.radius <= 3.2f);
}

// M-LAMPGLOW 37b: a MeshRef on the flickering entity has its instance emissive_tint scaled by light_flicker_emissive_scale.
namespace {

const asset::Guid kLampMesh{0x3d00000000000042ull, 0x8000000000000042ull};

struct LampRig : Rig {
    LampRig() {
        mesh_fixture::Builder box;
        box.add_node(-1);
        mesh_fixture::Box b;
        b.half = {0.5f, 0.5f, 0.5f};
        box.add_box(b);
        const Array<byte> blob = box.build();
        REQUIRE(store.upload(1, 1, blob.span()).has_value());
        extract.bind_mesh(kLampMesh, 1);
    }
    Entity lamp(math::vec3 emissive) {
        MeshRef m;
        m.mesh.guid = kLampMesh;
        m.emissive_tint = emissive;
        return world.spawn_with(LocalTransform{{0, 0, -5}, math::quat::identity(), {1, 1, 1}}, m);
    }
    math::vec3 tint() {
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        REQUIRE(scene.instances().size() == 1);
        return scene.instances()[0].emissive_tint;
    }
};

bool same_vec(math::vec3 a, math::vec3 b) { return same_bits(a.x, b.x) && same_bits(a.y, b.y) && same_bits(a.z, b.z); }

}  // namespace

TEST_CASE("light flicker extract: emissive_follow 1 scales the mesh emissive_tint by the helper at the frame clock") {
    LampRig r;
    const math::vec3 authored{2.0f, 1.5f, 0.5f};
    const Entity e = r.lamp(authored);
    LightFlicker3D f = fire();
    f.emissive_follow = 1.0f;
    r.world.add(e, f);
    bool varied = false;
    math::vec3 first{};
    for (int i = 0; i < 12; ++i) {
        const f64 t = r.next_clock();
        const f32 k = light_flicker_emissive_scale(f, t, e.index);
        const math::vec3 got = r.tint();
        CHECK(same_bits(got.x, authored.x * k));
        CHECK(same_bits(got.y, authored.y * k));
        CHECK(same_bits(got.z, authored.z * k));
        CHECK(k <= 1.0f);
        if (i == 0) first = got;
        else if (!same_vec(got, first)) varied = true;
    }
    CHECK(varied);
    // Nothing is written back.
    const MeshRef* m = r.world.try_get<MeshRef>(e);
    REQUIRE(m != nullptr);
    CHECK(same_vec(m->emissive_tint, authored));
    const LightFlicker3D* fl = r.world.try_get<LightFlicker3D>(e);
    REQUIRE(fl != nullptr);
    CHECK(same_bits(fl->emissive_follow, 1.0f));
}

TEST_CASE("light flicker extract: emissive_follow 0, disabled and no component leave the emissive_tint bit-identical") {
    const math::vec3 authored{2.0f, 1.5f, 0.5f};
    LampRig plain;
    plain.lamp(authored);
    LampRig steady;
    LightFlicker3D f = fire();
    steady.world.add(steady.lamp(authored), f);  // emissive_follow defaults to 0
    LampRig off;
    LightFlicker3D d = fire();
    d.emissive_follow = 1.0f;
    d.enabled = 0;
    off.world.add(off.lamp(authored), d);
    for (int i = 0; i < 6; ++i) {
        CHECK(same_vec(plain.tint(), authored));
        CHECK(same_vec(steady.tint(), authored));
        CHECK(same_vec(off.tint(), authored));
    }
}
