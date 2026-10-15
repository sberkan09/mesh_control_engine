// Ragdolls as a component (M-GAMEPLAY3D step 3): Ragdoll3D on a skinned stick figure: parts built
// from the skeleton (sockets ride along, the sidecar's overrides), a fall the mesh's bones follow,
// an impulse on one bone, the blend back to the animation, a mesh without a skeleton, the hash.

#include <doctest/doctest.h>

#include <cmath>

#include "kit_mesh_tree.h"
#include "mce/ecs/transform.h"
#include "mce/ecs/world.h"
#include "mce/kit/anim3d.h"
#include "mce/kit/bodies3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/ragdolls3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::kit;
using kit_test::write_mesh_tree;
using math::vec3;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
const asset::Guid kFigure{0x3d00000000000071ull, 0x8000000000000001ull};
const asset::Guid kBlock{0x3d00000000000072ull, 0x8000000000000001ull};

// A stick figure 2.1 m tall standing on y = 0, skinned to hips, spine, head, two legs and two feet;
// "tip" is a socket on the head that moves no vertex. `flags` sets per-joint ragdoll overrides.
struct FigureJoint {
    const char* name;
    i32 parent;  // index into this table
    vec3 at;     // model space
};
const FigureJoint kJoints[] = {
    {"hips", -1, {0, 1.0f, 0}},          {"spine", 0, {0, 1.1f, 0}},          {"head", 1, {0, 1.9f, 0}},
    {"leg_l", 0, {-0.12f, 0.95f, 0}},    {"foot_l", 3, {-0.12f, 0.1f, 0}},    {"leg_r", 0, {0.12f, 0.95f, 0}},
    {"foot_r", 5, {0.12f, 0.1f, 0}},     {"tip", 2, {0, 2.2f, 0}},
};
struct FigureBox {
    u32 joint;
    vec3 center, half;
};
const FigureBox kBoxes[] = {
    {0, {0, 1.0f, 0}, {0.18f, 0.1f, 0.1f}},          {1, {0, 1.5f, 0}, {0.15f, 0.38f, 0.1f}},
    {2, {0, 2.0f, 0}, {0.1f, 0.1f, 0.1f}},           {3, {-0.12f, 0.52f, 0}, {0.07f, 0.4f, 0.07f}},
    {4, {-0.12f, 0.05f, 0.05f}, {0.06f, 0.05f, 0.12f}}, {5, {0.12f, 0.52f, 0}, {0.07f, 0.4f, 0.07f}},
    {6, {0.12f, 0.05f, 0.05f}, {0.06f, 0.05f, 0.12f}},
};

Array<byte> figure_blob(std::initializer_list<std::pair<u32, u32>> flags = {}) {
    mesh_fixture::Builder b;
    const u32 armature = b.add_node(-1, {}, "armature");  // a non-joint root
    u32 node[8];
    for (u32 j = 0; j < 8; ++j) {
        const FigureJoint& fj = kJoints[j];
        const u32 parent = fj.parent < 0 ? armature : node[fj.parent];
        const vec3 parent_at = fj.parent < 0 ? vec3{} : kJoints[fj.parent].at;
        node[j] = b.add_node(static_cast<i32>(parent), fj.at - parent_at, fj.name);
    }
    for (const FigureBox& box : kBoxes) {
        mesh_fixture::Box mb;
        mb.center = box.center;
        mb.half = box.half;
        b.add_box(mb);
    }
    b.skin_submesh(0, {node[0], node[1], node[2], node[3], node[4], node[5], node[6], node[7]}, [&](vec3) { return kBoxes[0].joint; });
    for (u32 s = 1; s < 7; ++s) b.skin_more(s, 0, [&](vec3) { return kBoxes[s].joint; });
    for (const auto& [j, f] : flags) b.joints[j].ragdoll_flags = f;
    return b.build();
}

Array<byte> block_blob() {
    mesh_fixture::Builder b;
    b.add_node(-1, {}, "block");
    mesh_fixture::Box mb;
    mb.half = {0.5f, 0.5f, 0.5f};
    b.add_box(mb);
    return b.build();
}

struct Scene {
    asset::AssetSystem assets;
    World w;
    u64 t = 0;
    explicit Scene(const char* name, Span<const byte> figure) {
        const std::string figure_root = write_mesh_tree(name, kFigure, figure);
        const Array<byte> block = block_blob();
        const std::string block_root = write_mesh_tree((std::string(name) + "_block").c_str(), kBlock, block.span());
        const StringView roots[] = {StringView(figure_root.c_str()), StringView(block_root.c_str())};
        REQUIRE(assets.mount(Span<const StringView>(roots, 2)));
        w.register_components<LocalTransform>();
        install_ragdolls3d(w, &assets);
        BoxCollider3D floor;
        floor.half = {20.0f, 0.5f, 20.0f};
        w.spawn_with(LocalTransform{{0, -0.5f, 0}, math::quat::identity(), {1, 1, 1}}, floor);
    }
    void run(u32 n) {
        for (u32 i = 0; i < n; ++i) w.tick(kDt, t++);
    }
    Entity figure(vec3 at, asset::Guid mesh = kFigure) {
        MeshRef m;
        m.mesh.guid = mesh;
        return w.spawn_with(LocalTransform{at, math::quat::identity(), {1, 1, 1}}, m, Ragdoll3D{});
    }
    // An entity pinned to `bone` of `target`, so the test can see where the bone is drawn.
    Entity marker(Entity target, const char* bone) {
        AttachToBone a;
        a.target = target;
        a.bone = bone;
        return w.spawn_with(LocalTransform{}, a);
    }
    Ragdoll3D& rag(Entity e) { return *w.try_get_mut<Ragdoll3D>(e); }
    vec3 pos(Entity e) { return w.try_get<LocalTransform>(e)->position; }
    Physics3D& phys() { return w.resource<Physics3D>(); }
};

}  // namespace

TEST_CASE("ragdolls3d: built from the skeleton, falls, and the mesh's bones follow; sockets ride along") {
    const Array<byte> blob = figure_blob();
    Scene s("ragdolls3d_fall", blob.span());
    const Entity e = s.figure({0, 0, 0});
    const Entity tip = s.marker(e, "tip");
    const Entity foot = s.marker(e, "foot_l");
    s.run(1);
    CHECK(s.rag(e).parts == 0);  // not active: the animation (rest pose) poses it
    CHECK(s.pos(tip).y == doctest::Approx(2.2f));

    s.rag(e).active = 1;
    s.rag(e).impulse = {0, 0, 30.0f};  // a shove forward (+z), over the whole body
    s.run(1);
    CHECK(s.rag(e).parts == 7);  // hips, spine, head, legs, feet; the tip rides the head
    CHECK(s.phys().world.stats().ragdolls == 1);
    CHECK(s.rag(e).blend == 1.0f);
    CHECK(s.rag(e).impulse.z == 0.0f);  // applied once
    CHECK(s.pos(tip).y > 2.1f);         // still standing, near the rest pose
    s.run(240);
    CHECK(s.rag(e).settled == 1);
    CHECK(s.pos(tip).y < 0.6f);  // lying down
    CHECK(s.pos(tip).z > 0.8f);  // fell forward
    CHECK(s.pos(foot).y < 0.4f);
    CHECK(s.pos(foot).y > -0.05f);  // on the floor, not through it
    CHECK(s.rag(e).root_position.y < 0.5f);
    CHECK(s.pos(e).y == 0.0f);  // the entity itself stays put
}

TEST_CASE("ragdolls3d: an impulse on one bone, then the blend back to the animation") {
    const Array<byte> blob = figure_blob();
    Scene s("ragdolls3d_blend", blob.span());
    const Entity e = s.figure({2, 0, 0});
    const Entity tip = s.marker(e, "tip");
    s.rag(e).active = 1;
    s.rag(e).impulse = {-25.0f, 0, 0};
    s.rag(e).impulse_bone = "tip";  // lands on the head (the tip rides it)
    s.run(180);
    CHECK(s.pos(tip).x < 1.2f);  // knocked over toward -x by the head
    CHECK(s.pos(tip).y < 0.6f);

    s.rag(e).active = 0;
    s.rag(e).blend_time = 0.25f;  // 15 steps
    s.run(1);
    CHECK(s.rag(e).parts == 0);
    CHECK(s.phys().world.stats().ragdolls == 0);
    CHECK(s.rag(e).blend == 1.0f);  // the frozen pose, about to fade
    const f32 lying = s.pos(tip).y;
    CHECK(lying < 0.6f);
    s.run(7);
    CHECK(s.rag(e).blend > 0.4f);
    CHECK(s.rag(e).blend < 0.6f);
    CHECK(s.pos(tip).y > lying + 0.3f);  // half-way up
    s.run(10);
    CHECK(s.rag(e).blend == 0.0f);
    CHECK(s.pos(tip).x == doctest::Approx(2.0f));
    CHECK(s.pos(tip).y == doctest::Approx(2.2f));

    // Limp again: a fresh ragdoll from the pose as drawn.
    s.rag(e).active = 1;
    s.run(1);
    CHECK(s.rag(e).parts == 7);
}

TEST_CASE("ragdolls3d: the sidecar's overrides exclude and include bones") {
    using asset::MeshRagdoll::Exclude;
    using asset::MeshRagdoll::Include;
    const Array<byte> blob = figure_blob({{4, Exclude}, {6, Exclude}, {7, Include}});
    Scene s("ragdolls3d_overrides", blob.span());
    const Entity e = s.figure({0, 0, 0});
    s.rag(e).active = 1;
    s.run(1);
    CHECK(s.rag(e).parts == 6);  // no feet (they ride the legs), a ball for the tip
}

TEST_CASE("ragdolls3d: a mesh without a skeleton is refused once; removal frees the bodies") {
    const Array<byte> blob = figure_blob();
    Scene s("ragdolls3d_refused", blob.span());
    const Entity block = s.figure({3, 0.5f, 0}, kBlock);
    s.rag(block).active = 1;
    s.run(3);
    CHECK(s.rag(block).parts == 0);
    CHECK(s.phys().world.stats().ragdolls == 0);

    const Entity e = s.figure({0, 0, 0});
    s.rag(e).active = 1;
    s.run(2);
    CHECK(s.phys().world.stats().ragdolls == 1);
    s.w.remove<Ragdoll3D>(e);
    s.run(1);
    CHECK(s.phys().world.stats().ragdolls == 0);
    const Entity f = s.figure({0, 0, 3});
    s.rag(f).active = 1;
    s.run(2);
    CHECK(s.phys().world.stats().ragdolls == 1);
    s.w.destroy(f);
    s.run(1);
    CHECK(s.phys().world.stats().ragdolls == 0);
}

TEST_CASE("ragdolls3d: the replay hash holds with ragdolls") {
    const Array<byte> blob = figure_blob();
    auto run = [&] {
        Scene s("ragdolls3d_hash", blob.span());
        const Entity a = s.figure({0, 0, 0});
        const Entity b = s.figure({0.3f, 0, 0.4f});
        s.rag(a).active = 1;
        s.rag(a).impulse = {10.0f, 0, 5.0f};
        s.run(30);
        s.rag(b).active = 1;
        s.run(150);
        s.rag(a).active = 0;
        s.run(30);
        return s.w.state_hash();
    };
    CHECK(run() == run());
}
