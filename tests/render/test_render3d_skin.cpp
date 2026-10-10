// Optional skinning (FR-R3D-060), headless: a CHNK_MESH with a skin section round-trips through
// MeshView and MeshStore, skin_palette maps bind space through the posed joints, Scene3D builds
// one palette per skinned instance and moves its bounds with the pose, an instance can opt out of
// its skeleton, and meshes without skins are untouched.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

#include "mce/render/mesh.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::render;
using math::mat4;
using math::vec3;

namespace {

// A two-bone "arm" standing on the origin: joint A at the root, joint B 1 m up; one 2 m box
// skinned to them (lower half to A, upper half to B) and one rigid box on the mesh node.
struct Arm {
    mesh_fixture::Builder b;
    u32 root = 0, joint_a = 0, joint_b = 0, mesh_node = 0;

    Arm() {
        root = b.add_node(-1);
        joint_a = b.add_node(static_cast<i32>(root));
        joint_b = b.add_node(static_cast<i32>(joint_a), {0, 1, 0});
        mesh_node = b.add_node(static_cast<i32>(root));
        mesh_fixture::Box bone;
        bone.center = {0, 1, 0};
        bone.half = {0.2f, 1.0f, 0.2f};
        bone.node = mesh_node;
        b.add_box(bone);
        mesh_fixture::Box prop;
        prop.center = {1, 0.25f, 0};
        prop.half = {0.25f, 0.25f, 0.25f};
        prop.node = mesh_node;
        b.add_box(prop);
        b.skin_submesh(0, {joint_a, joint_b}, [](vec3 p) { return p.y < 1.0f ? 0u : 1u; });
    }
};

// The rest pose with joint B bent 90 degrees about +Z (the upper half swings toward -X).
void bent_pose(const GpuMesh& m, u32 joint_b, Span<mat4> out) {
    pose_nodes(m, ~0u, 0.0f, out);
    const i32 parent = m.nodes[joint_b].parent;
    out[joint_b] = out[static_cast<usize>(parent)] *
                   math::trs({0, 1, 0}, math::quat_from_axis_angle({0, 0, 1}, math::kPi * 0.5f), {1, 1, 1});
}

}  // namespace

TEST_CASE("skin: the skin section round-trips; MeshStore keeps skins, joints and per-submesh skins") {
    const Arm arm;
    const Array<byte> blob = arm.b.build();
    auto mv = asset::MeshView::read(blob.span());
    REQUIRE(mv.has_value());
    const asset::MeshView& v = mv.value();
    CHECK(v.skinned());
    REQUIRE(v.skins.size() == 1);
    REQUIRE(v.joints.size() == 2);
    CHECK(v.joints[1].node == arm.joint_b);
    CHECK(v.submesh_skins[0] == 0);
    CHECK(v.submesh_skins[1] == -1);
    CHECK(v.skin_vertices.size() == v.vertices.size());

    MeshStore store;
    REQUIRE(store.upload(1, 1, blob.span()).has_value());
    const GpuMesh& m = *store.get(1);
    CHECK(m.skinned());
    CHECK(m.submeshes[0].skin == 0);
    CHECK(m.submeshes[1].skin == ~0u);
    REQUIRE(m.joints.size() == 2);
    CHECK(m.joints[0].influences);
    CHECK(m.joints[1].bounds.max.y == doctest::Approx(2.0f));
    CHECK(m.find_node("") == 0);  // fixture nodes are unnamed: the first match

    // Without a skin section nothing changes.
    mesh_fixture::Builder plain;
    plain.add_node(-1);
    plain.add_box({{0, 0.5f, 0}, {0.5f, 0.5f, 0.5f}});
    REQUIRE(store.upload(2, 1, plain.build().span()).has_value());
    CHECK_FALSE(store.get(2)->skinned());
    CHECK(store.get(2)->submeshes[0].skin == ~0u);
}

TEST_CASE("skin: the palette is the identity at rest and follows a bent joint") {
    const Arm arm;
    MeshStore store;
    REQUIRE(store.upload(1, 1, arm.b.build().span()).has_value());
    const GpuMesh& m = *store.get(1);

    mat4 palette[2];
    skin_palette(m, 0, {}, Span<mat4>(palette, 2));  // empty pose = rest pose
    for (const mat4& p : palette) {
        const vec3 q = math::transform_point(p, {0.3f, 1.7f, -0.2f});
        CHECK(q.x == doctest::Approx(0.3f));
        CHECK(q.y == doctest::Approx(1.7f));
        CHECK(q.z == doctest::Approx(-0.2f));
    }

    mat4 pose[4];
    bent_pose(m, arm.joint_b, Span<mat4>(pose, 4));
    skin_palette(m, 0, Span<const mat4>(pose, 4), Span<mat4>(palette, 2));
    const vec3 tip = math::transform_point(palette[1], {0, 2, 0});  // bind-space top of the bone
    CHECK(tip.x == doctest::Approx(-1.0f));
    CHECK(tip.y == doctest::Approx(1.0f));
    const vec3 foot = math::transform_point(palette[0], {0, 0, 0});  // joint A did not move
    CHECK(foot.y == doctest::Approx(0.0f));

    const math::aabb3 bent = skin_bounds(m, 0, Span<const mat4>(palette, 2));
    CHECK(bent.min.x == doctest::Approx(-1.0f).epsilon(1e-3));  // the bone tip swung to -X
    CHECK(bent.max.y == doctest::Approx(1.2f).epsilon(1e-3));
}

TEST_CASE("skin: Scene3D builds one palette per skinned instance; instances can opt out") {
    const Arm arm;
    MeshStore store;
    REQUIRE(store.upload(1, 1, arm.b.build().span()).has_value());
    mesh_fixture::Builder plain;
    plain.add_node(-1);
    plain.add_box({{0, 0.5f, 0}, {0.5f, 0.5f, 0.5f}});
    REQUIRE(store.upload(2, 1, plain.build().span()).has_value());
    const GpuMesh& m = *store.get(1);
    mat4 pose[4];
    bent_pose(m, arm.joint_b, Span<mat4>(pose, 4));

    Scene3D scene(store, mem::heap());
    scene.begin(orbit_camera({0, 1, 0}, 0.0f, math::radians(20.0f), 8.0f), {320, 180});
    MeshInstance skinned;
    skinned.mesh = 1;
    skinned.pose = Span<const mat4>(pose, 4);
    scene.add(skinned);
    MeshInstance stiff = skinned;  // same mesh, skeleton off: the bind pose, drawn rigidly
    stiff.skinning = false;
    stiff.world = math::translation({5, 0, 0});
    scene.add(stiff);
    MeshInstance box;
    box.mesh = 2;
    scene.add(box);
    scene.build();

    CHECK(scene.stats().skinned_instances == 1);
    CHECK(scene.joint_palette().size() == 2);
    const vec3 tip = math::transform_point(scene.joint_palette()[1], {0, 2, 0});
    CHECK(tip.x == doctest::Approx(-1.0f));

    u32 seen = 0;
    for (const Scene3D::Draw& d : scene.shadow_draws()) {
        const GpuSubmesh& sm = d.mesh->submeshes[d.submesh];
        if (d.instance == 0 && d.submesh == 0) {  // skinned bone: world model, palette, posed bounds
            CHECK(d.palette == 0);
            CHECK(d.center.x < -0.2f);
            CHECK(d.model.c[3][0] == doctest::Approx(0.0f));
            ++seen;
        } else if (d.instance == 1 && d.submesh == 0) {  // opted out: bind pose under world
            CHECK(d.palette == ~0u);
            CHECK(d.center.x == doctest::Approx(5.0f));
            CHECK(d.center.y == doctest::Approx(1.0f));
            ++seen;
        } else {  // rigid submeshes and the plain mesh draw exactly as before
            CHECK(d.palette == ~0u);
            CHECK(sm.skin == ~0u);
            ++seen;
        }
    }
    CHECK(seen == 5);
    CHECK(scene.scene_bounds().min.x == doctest::Approx(-1.0f).epsilon(1e-3));
}

TEST_CASE("skin: MeshView rejects corrupt skin sections") {
    const Arm arm;
    const Array<byte> good = arm.b.build();
    const auto* h = reinterpret_cast<const asset::MeshHeader*>(good.data());
    const auto* sh = reinterpret_cast<const asset::MeshSkinHeader*>(good.data() + h->skin_offset);

    {  // a vertex naming joint 7 of a 2-joint skin
        Array<byte> bad = good;
        auto* sv = reinterpret_cast<asset::MeshSkinVertex*>(bad.data() + sh->vertices_offset);
        sv[0].joints[0] = 7;
        CHECK_FALSE(asset::MeshView::read(bad.span()).has_value());
    }
    {  // a joint driven by a node that does not exist
        Array<byte> bad = good;
        auto* j = reinterpret_cast<asset::MeshJoint*>(bad.data() + sh->joints_offset);
        j[1].node = 99;
        CHECK_FALSE(asset::MeshView::read(bad.span()).has_value());
    }
    {  // a submesh naming a missing skin
        Array<byte> bad = good;
        auto* ss = reinterpret_cast<i32*>(bad.data() + sh->submesh_skins_offset);
        ss[1] = 3;
        CHECK_FALSE(asset::MeshView::read(bad.span()).has_value());
    }
    {  // a skin offset on a mesh that is not flagged skinned
        Array<byte> bad = good;
        auto* hh = reinterpret_cast<asset::MeshHeader*>(bad.data());
        hh->flags &= ~u32(asset::MeshFlags::Skinned);
        CHECK_FALSE(asset::MeshView::read(bad.span()).has_value());
    }
}

TEST_CASE("skin: playback events fire once per pass, across loops, for the current clip only") {
    mesh_fixture::Builder b;
    const u32 n = b.add_node(-1);
    b.add_box({{0, 0.5f, 0}, {0.5f, 0.5f, 0.5f}});
    b.add_spin_clip(n, 1.0f, "walk");
    b.add_spin_clip(n, 2.0f, "run");
    MeshStore store;
    REQUIRE(store.upload(1, 1, b.build().span()).has_value());
    const GpuMesh& m = *store.get(1);
    const ClipEvent markers[] = {{0, 0.0f, 10}, {0, 0.5f, 11}, {1, 0.25f, 20}};
    u64 out[8];
    AnimationPlayer p;
    p.play(0);
    auto fired = [&](f32 dt) {
        p.advance(dt);
        return p.events(m, Span<const ClipEvent>(markers, 3), Span<u64>(out, 8));
    };
    REQUIRE(fired(0.25f) == 1);  // [0, 0.25): the marker at 0
    CHECK(out[0] == 10);
    REQUIRE(fired(0.25f) == 0);  // [0.25, 0.5): 0.5 is not inside yet
    REQUIRE(fired(0.25f) == 1);  // [0.5, 0.75)
    CHECK(out[0] == 11);
    REQUIRE(fired(0.5f) == 1);   // [0.75, 1.25): the loop wraps past 0 again
    CHECK(out[0] == 10);
    REQUIRE(fired(2.0f) == 4);   // two whole loops: each marker twice, in marker order
    CHECK(out[0] == 10);
    CHECK(out[2] == 11);
    p.play(1, 0.2f);             // the faded-out clip's markers no longer fire
    REQUIRE(fired(0.5f) == 1);
    CHECK(out[0] == 20);
}
