// GiVisibility3D (M-GI2 50h): component defaults, save round trip, the Kit3DExtract feed onto render::GiVolume (read on
// the GiVolume3D's entity only; absent or disabled leaves the carrier defaults; max_offset clamps; a bake is of the
// authored box), the bake-format stamp, the following reach, and that the extract writes nothing into the world.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "mce/containers/array.h"
#include "mce/ecs/save.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/gi_visibility.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/render/gi_probes.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;

namespace {

const asset::Guid kBoxMesh{0x43d0000000000052ull, 0x8000000000000053ull};

struct Rig {
    render::MeshStore store;
    World world;
    Kit3DExtract extract;
    render::Scene3D scene;
    render::Camera3D cam;
    Entity camera_e;
    Entity gi_e;

    Rig() : scene(store, mem::heap()) {
        world.register_components<SaveKey>();
        install_kit3d(world, nullptr);
        extract.init(store, nullptr);
        mesh_fixture::Builder box;
        box.add_node(-1);
        mesh_fixture::Box b;
        b.half = {0.5f, 0.5f, 0.5f};
        box.add_box(b);
        const Array<byte> blob = box.build();
        REQUIRE(store.upload(1, 1, blob.span()).has_value());
        extract.bind_mesh(kBoxMesh, 1);
        camera_e = world.spawn_with(LocalTransform{}, Camera3DComponent{});
        gi_e = world.spawn_with(LocalTransform{}, GiVolume3D{});
    }
    const render::GiVolume& frame() {
        REQUIRE(extract.camera(world, 1.0f, cam));
        scene.begin(cam, {320, 180});
        extract.extract(world, scene, 1.0f);
        return scene.gi_volume();
    }
    void move_camera(math::vec3 p) { world.try_get_mut<LocalTransform>(camera_e)->position = p; }
};

bool defaults(const render::GiVolume& g) {
    const render::GiVolume d;
    return g.visibility == d.visibility && g.relocate == d.relocate && g.max_offset == d.max_offset &&
           g.follow == d.follow;
}

}  // namespace

TEST_CASE("gi visibility: the component's defaults") {
    const GiVisibility3D v;
    CHECK(v.enabled == 1);
    CHECK(v.relocate == 0);
    CHECK(v.max_offset == 0.4f);
    CHECK(v.follow_camera == 0);
    CHECK(v.bake_format == 0);
    CHECK(sizeof(GiVisibility3D) == 20);
}

TEST_CASE("gi visibility: the component round-trips save and load") {
    Array<byte> bytes;
    u64 hash_a = 0;
    {
        Rig a;
        a.world.add(a.gi_e, SaveKey{7});
        GiVisibility3D v;
        v.enabled = 0;
        v.relocate = 1;
        v.max_offset = 0.25f;
        v.follow_camera = 1;
        v.bake_format = 1;
        a.world.add(a.gi_e, v);
        save::write_save(a.world, {}, bytes);
        hash_a = a.world.state_hash();
    }
    Rig b;
    b.world.add(b.gi_e, SaveKey{7});
    b.world.add(b.gi_e, GiVisibility3D{});
    REQUIRE(save::read_save(b.world, bytes.span()));
    const GiVisibility3D& v = *b.world.try_get<GiVisibility3D>(b.gi_e);
    CHECK(v.enabled == 0);
    CHECK(v.relocate == 1);
    CHECK(v.max_offset == 0.25f);
    CHECK(v.follow_camera == 1);
    CHECK(v.bake_format == 1);
    CHECK(b.world.state_hash() == hash_a);
}

TEST_CASE("gi visibility extract: without the component the carrier keeps its defaults") {
    Rig r;
    const render::GiVolume& g = r.frame();
    REQUIRE(g.enabled);
    CHECK(defaults(g));
    CHECK(render::gi_volume_size(g).width == g.counts[0] * 4u);
}

TEST_CASE("gi visibility extract: the fields map one to one") {
    Rig r;
    GiVisibility3D v;
    v.relocate = 1;
    v.max_offset = 0.3f;
    v.follow_camera = 1;
    r.world.add(r.gi_e, v);
    const render::GiVolume& g = r.frame();
    REQUIRE(g.enabled);
    CHECK(g.visibility);
    CHECK(g.relocate);
    CHECK(g.max_offset == 0.3f);
    CHECK(g.follow == g.updates);  // apply_quality keeps follow only where the volume updates
    CHECK(render::gi_texels_per_probe(g) == 10u);
    CHECK(render::gi_volume_size(g).width == g.counts[0] * 10u);

    // enabled alone: the 10-texel record, nothing else
    Rig s;
    s.world.add(s.gi_e, GiVisibility3D{});
    const render::GiVolume& h = s.frame();
    CHECK(h.visibility);
    CHECK_FALSE(h.relocate);
    CHECK_FALSE(h.follow);
    CHECK(h.max_offset == 0.4f);
}

TEST_CASE("gi visibility extract: a disabled component, or one on another entity, is ignored") {
    Rig off;
    GiVisibility3D v;
    v.enabled = 0;
    v.relocate = 1;
    v.follow_camera = 1;
    off.world.add(off.gi_e, v);
    CHECK(defaults(off.frame()));

    Rig other;
    GiVisibility3D w;
    w.relocate = 1;
    w.follow_camera = 1;
    other.world.spawn_with(LocalTransform{}, w);  // not the GiVolume3D's entity
    other.world.add(other.camera_e, w);
    CHECK(defaults(other.frame()));
}

TEST_CASE("gi visibility extract: max_offset clamps to 0..0.45 and NaN is the default") {
    const f32 cases[][2] = {{-1.0f, 0.0f}, {0.0f, 0.0f}, {0.45f, 0.45f}, {2.0f, 0.45f}, {std::nanf(""), 0.4f}};
    for (const auto& c : cases) {
        Rig r;
        GiVisibility3D v;
        v.max_offset = c[0];
        r.world.add(r.gi_e, v);
        CHECK(r.frame().max_offset == c[1]);
    }
}

TEST_CASE("gi visibility extract: a bake of the authored box never follows") {
    Rig r;
    GiVisibility3D v;
    v.relocate = 1;
    v.follow_camera = 1;
    r.world.add(r.gi_e, v);
    r.extract.set_bake_mode(true);
    const render::GiVolume& g = r.frame();
    r.extract.set_bake_mode(false);
    CHECK(g.visibility);
    CHECK(g.relocate);
    CHECK_FALSE(g.follow);
}

TEST_CASE("gi visibility: a bake is used only with the stamp of this build's record format") {
    CHECK(gi_visibility_bake_ok(nullptr));
    GiVisibility3D v;
    CHECK_FALSE(gi_visibility_bake_ok(&v));  // 10-texel record, unstamped
    v.bake_format = kGiRecordFormat + 1;
    CHECK_FALSE(gi_visibility_bake_ok(&v));
    v.bake_format = kGiRecordFormat;
    CHECK(gi_visibility_bake_ok(&v));
    v.bake_format = 0;
    v.enabled = 0;  // the 4-texel layout carries no record
    CHECK(gi_visibility_bake_ok(&v));

    // through the extract: the bake reaches the carrier only with the stamp (a plain volume's bake always does)
    const asset::Guid guid{0x1ull, 0x8000000000000002ull};
    Rig plain;
    GiVolume3D gv;
    gv.baked.guid = guid;
    *plain.world.try_get_mut<GiVolume3D>(plain.gi_e) = gv;
    const u32 plain_baked = plain.frame().baked;
    Rig stamped, unstamped;
    for (Rig* r : {&stamped, &unstamped}) {
        *r->world.try_get_mut<GiVolume3D>(r->gi_e) = gv;
        r->world.add(r->gi_e, GiVisibility3D{});
    }
    stamped.world.try_get_mut<GiVisibility3D>(stamped.gi_e)->bake_format = kGiRecordFormat;
    CHECK(stamped.frame().baked == plain_baked);
    CHECK(unstamped.frame().baked == ~0u);
}

constexpr std::size_t kNoReach = static_cast<std::size_t>(-1);

TEST_CASE("gi visibility extract: the following reach contains the snapped box") {
    // The volume sits 100 m away; a mesh 6 m behind the camera is outside the view and outside the authored box's reach,
    // and is submitted only because the box follows the camera.
    auto submitted = [](bool follow) {
        Rig r;
        *r.world.try_get_mut<LocalTransform>(r.gi_e) = LocalTransform{{100.0f, 0.0f, 0.0f}, math::quat::identity(), {1, 1, 1}};
        GiVisibility3D v;
        v.follow_camera = follow ? 1 : 0;
        r.world.add(r.gi_e, v);
        MeshRef m;
        m.mesh.guid = kBoxMesh;
        m.cull_radius = 1.0f;  // the extract culls only meshes that name a radius
        r.world.spawn_with(LocalTransform{{0.0f, 0.0f, 6.0f}, math::quat::identity(), {1, 1, 1}}, m);
        const render::GiVolume& g = r.frame();
        REQUIRE(g.enabled);
        if (!g.updates) return kNoReach;
        return r.scene.instances().size();
    };
    const std::size_t with = submitted(true), without = submitted(false);
    if (with == kNoReach) return;  // a quality tier without runtime captures: no reach to test
    CHECK(without == 0);
    CHECK(with == 1);
}

TEST_CASE("gi visibility extract: the extract writes nothing into the world") {
    auto run = [](bool extract) {
        Rig r;
        GiVisibility3D v;
        v.relocate = 1;
        v.follow_camera = 1;
        r.world.add(r.gi_e, v);
        for (u32 i = 0; i < 100; ++i) {
            r.move_camera({f32(i) * 1.7f, 0.0f, f32(i) * -0.9f});
            if (extract) r.frame();
            else REQUIRE(r.extract.camera(r.world, 1.0f, r.cam));
        }
        return r.world.state_hash();
    };
    CHECK(run(true) == run(false));
}

TEST_CASE("gi visibility: GiVolume3D's column is the same with and without the component") {
    Rig a, b;
    b.world.add(b.gi_e, GiVisibility3D{});
    a.frame();
    b.frame();
    const GiVolume3D& ga = *a.world.try_get<GiVolume3D>(a.gi_e);
    const GiVolume3D& gb = *b.world.try_get<GiVolume3D>(b.gi_e);
    // both copies come from the same writer, padding included
    // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison)
    CHECK(std::memcmp(&ga, &gb, sizeof(GiVolume3D)) == 0);
    CHECK(sizeof(GiVolume3D) == 88);
}
