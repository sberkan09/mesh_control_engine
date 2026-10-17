// Automatic instancing (M-VISUAL3 5a), headless: Scene3D's runs cover the opaque list exactly, gather the
// copies of one rigid submesh with one material and tint, leave tinted, moving, blended and skinned copies
// out, count every draw once by cause, and the caster lists of each cascade form their own runs.

#include <doctest/doctest.h>

#include "mce/render/mesh.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::render;
using math::vec3;

namespace {

Array<byte> box_mesh(u32 flags = 0, f32 half = 0.5f) {
    mesh_fixture::Builder b;
    b.add_node(-1);
    mesh_fixture::Box box;
    box.half = {half, half, half};
    box.flags = flags;
    b.add_box(box);
    return b.build();
}

// A skinned mesh (one skinned submesh, one rigid one on the same node), as test_render3d_skin's arm.
Array<byte> skinned_mesh() {
    mesh_fixture::Builder b;
    const u32 root = b.add_node(-1);
    const u32 a = b.add_node(static_cast<i32>(root));
    const u32 j = b.add_node(static_cast<i32>(a), {0, 1, 0});
    const u32 node = b.add_node(static_cast<i32>(root));
    mesh_fixture::Box bone;
    bone.center = {0, 1, 0};
    bone.half = {0.2f, 1.0f, 0.2f};
    bone.node = node;
    b.add_box(bone);
    mesh_fixture::Box prop;
    prop.center = {1, 0.25f, 0};
    prop.half = {0.25f, 0.25f, 0.25f};
    prop.node = node;
    b.add_box(prop);
    b.skin_submesh(0, {a, j}, [](vec3 p) { return p.y < 1.0f ? 0u : 1u; });
    return b.build();
}

MeshInstance at(u32 mesh, vec3 p) {
    MeshInstance i;
    i.mesh = mesh;
    i.world = math::translation(p);
    return i;
}

Camera3D camera() {
    Camera3D c;  // at the origin, looking down -Z
    c.near_z = 0.1f;
    c.far_z = 200.0f;
    return c;
}

// Every run is contiguous, they cover [0, opaque_count()) in order, and every draw is counted once.
void check_cover(const Scene3D& s) {
    u32 next = 0;
    for (const DrawRun& r : s.opaque_runs()) {
        CHECK(r.first == next);
        CHECK(r.count >= 1);
        next += r.count;
        for (u32 k = 1; k < r.count; ++k) {  // one key per run
            const Scene3D::Draw& a = s.draws()[r.first];
            const Scene3D::Draw& b = s.draws()[r.first + k];
            CHECK(a.mesh == b.mesh);
            CHECK(a.submesh == b.submesh);
            CHECK(a.lod == b.lod);
        }
    }
    CHECK(next == s.opaque_count());
    const Scene3D::Stats& st = s.stats();
    CHECK(st.draws_instanced + st.not_instanced_blended + st.not_instanced_terrain + st.not_instanced_skinned +
              st.not_instanced_moving + st.not_instanced_single + st.not_instanced_disabled ==
          s.draws().size());
    CHECK(st.not_instanced_unique_mesh + st.not_instanced_short <= st.not_instanced_single);  // M-CONTENT 2
}

u32 run_containing(const Scene3D& s, u32 instance) {
    for (const DrawRun& r : s.opaque_runs())
        for (u32 k = 0; k < r.count; ++k)
            if (s.draws()[r.first + k].instance == instance) return r.count;
    return 0;
}

}  // namespace

TEST_CASE("instancing: runs cover the opaque list exactly and gather the copies of one mesh") {
    MeshStore store;
    REQUIRE(store.upload(1, 1, box_mesh().span()).has_value());
    REQUIRE(store.upload(2, 1, box_mesh(asset::MaterialFlags::AlphaBlend).span()).has_value());
    REQUIRE(store.upload(3, 1, box_mesh(0, 0.3f).span()).has_value());
    Scene3D scene(store, mem::heap());
    scene.begin(camera(), {256, 256});
    for (int i = 0; i < 6; ++i) {
        scene.add(at(1, {f32(i % 3) - 1.0f, 0.0f, -5.0f - f32(i)}));     // six copies of mesh 1
        if (i % 2 == 0) scene.add(at(3, {0.0f, 1.5f, -5.5f - f32(i)}));  // three of mesh 3 between them
    }
    scene.add(at(2, {0, 0, -6}));  // blended: after the opaques, never in a run
    scene.add(at(2, {0, 0, -7}));
    scene.build();
    REQUIRE(scene.draws().size() == 11);
    CHECK(scene.opaque_count() == 9);
    check_cover(scene);
    CHECK(scene.stats().runs_instanced == 2);
    CHECK(scene.stats().draws_instanced == 9);
    CHECK(scene.stats().not_instanced_blended == 2);
    CHECK(scene.opaque_runs().size() == 2);
    // The first group is the nearest draw's (mesh 1 at z = -5); its copies keep their front-to-back order.
    const DrawRun first = scene.opaque_runs()[0];
    CHECK(scene.draws()[first.first].mesh == store.get(1));
    for (u32 k = 1; k < first.count; ++k)
        CHECK(scene.draws()[first.first + k - 1].view_depth <= scene.draws()[first.first + k].view_depth);

    // The same draws as without instancing, in the sort's order there.
    Scene3D off(store, mem::heap());
    off.set_instancing({false, 2});
    off.begin(camera(), {256, 256});
    for (const MeshInstance& i : scene.instances()) off.add(i);
    off.build();
    REQUIRE(off.draws().size() == scene.draws().size());
    CHECK(off.opaque_runs().size() == off.opaque_count());
    CHECK(off.stats().runs_instanced == 0);
    CHECK(off.stats().not_instanced_disabled == 11);
    check_cover(off);
    u32 seen_on = 0, seen_off = 0;
    for (u32 i = 0; i < scene.draws().size(); ++i) {
        seen_on += scene.draws()[i].instance * 7 + 1;
        seen_off += off.draws()[i].instance * 7 + 1;
    }
    CHECK(seen_on == seen_off);
    for (u32 i = scene.opaque_count(); i < scene.draws().size(); ++i)
        CHECK(scene.draws()[i].instance == off.draws()[i].instance);
}

TEST_CASE("instancing: a tinted, moving, blended or skinned copy breaks the run") {
    MeshStore store;
    REQUIRE(store.upload(1, 1, box_mesh().span()).has_value());
    REQUIRE(store.upload(4, 1, skinned_mesh().span()).has_value());
    MaterialOverride blend;
    blend.params = store.get(1)->submeshes[0].material;
    blend.params.flags |= asset::MaterialFlags::AlphaBlend;
    blend.albedo_key = store.get(1)->submeshes[0].albedo_key;
    const math::mat4 before = math::translation({0.5f, 0.0f, -9.0f});
    Scene3D scene(store, mem::heap());
    PostFx post;
    post.taa = true;  // the velocity prepass runs: a moving copy draws on its own
    scene.set_post(post);
    scene.begin(camera(), {256, 256});
    for (u32 i = 0; i < 7; ++i) {
        MeshInstance m = at(1, {f32(i) - 3.0f, 0.0f, -9.0f});
        if (i == 1) m.tint = {1.0f, 0.5f, 0.5f, 1.0f};
        if (i == 2) m.prev_world = &before;
        if (i == 3) m.material = &blend;
        if (i == 4) m.emissive_tint = {2.0f, 2.0f, 2.0f};
        scene.add(m);
    }
    for (u32 i = 0; i < 3; ++i) scene.add(at(4, {f32(i) * 2.0f - 2.0f, -1.0f, -12.0f}));
    scene.build();
    check_cover(scene);
    CHECK(run_containing(scene, 0) == 3);  // 0, 5 and 6
    CHECK(run_containing(scene, 5) == 3);
    CHECK(run_containing(scene, 1) == 1);                               // tinted
    CHECK(run_containing(scene, 2) == 1);                               // moving
    CHECK(run_containing(scene, 4) == 1);                               // another emissive tint
    CHECK(run_containing(scene, 3) == 0);                               // blended: not opaque
    for (u32 i = 7; i < 10; ++i) CHECK(run_containing(scene, i) == 1);  // skinned
    const Scene3D::Stats& st = scene.stats();
    CHECK(st.runs_instanced == 1);
    CHECK(st.draws_instanced == 3);
    CHECK(st.not_instanced_blended == 1);
    CHECK(st.not_instanced_moving == 1);
    CHECK(st.not_instanced_skinned == 6);  // both submeshes of each skinned copy
    CHECK(st.not_instanced_single == 2);   // the tinted and the glowing copy
    CHECK(st.not_instanced_terrain == 0);

    // The same moved copy joins the run when no velocity prepass reads its previous transform.
    Scene3D still(store, mem::heap());
    still.begin(camera(), {256, 256});
    for (u32 i = 0; i < 7; ++i) {
        MeshInstance m = scene.instances()[i];
        still.add(m);
    }
    still.build();
    CHECK(run_containing(still, 2) == 4);  // 0, 2, 5 and 6
    CHECK(still.stats().not_instanced_moving == 0);
}

TEST_CASE("instancing: min_count keeps short groups single and in the sort's order") {
    MeshStore store;
    REQUIRE(store.upload(1, 1, box_mesh().span()).has_value());
    REQUIRE(store.upload(3, 1, box_mesh(0, 0.3f).span()).has_value());
    Scene3D scene(store, mem::heap());
    scene.set_instancing({true, 3});
    scene.begin(camera(), {256, 256});
    scene.add(at(1, {0, 0, -5}));
    scene.add(at(3, {0, 0, -6}));
    scene.add(at(1, {0, 0, -7}));  // two copies of mesh 1: under min_count
    scene.add(at(3, {0, 0, -8}));
    scene.add(at(3, {0, 0, -9}));  // three of mesh 3: a run
    scene.build();
    check_cover(scene);
    CHECK(scene.stats().runs_instanced == 1);
    CHECK(scene.stats().draws_instanced == 3);
    CHECK(scene.stats().not_instanced_single == 2);
    const u32 expect[] = {0, 1, 3, 4, 2};  // mesh 1 at -5, the mesh-3 run from -6, mesh 1 at -7
    for (u32 i = 0; i < 5; ++i) CHECK(scene.draws()[i].instance == expect[i]);
}

TEST_CASE("instancing: each cascade's casters form their own runs; tint does not split them") {
    MeshStore store;
    REQUIRE(store.upload(1, 1, box_mesh().span()).has_value());
    REQUIRE(store.upload(3, 1, box_mesh(0, 0.3f).span()).has_value());
    REQUIRE(store.upload(4, 1, skinned_mesh().span()).has_value());
    Scene3D scene(store, mem::heap());
    scene.begin(orbit_camera({0, 0, 0}, 0.3f, 0.6f, 14.0f), {320, 200});
    ShadowSettings ss;
    ss.cascades = 3;
    ss.distance = 40.0f;
    scene.set_shadows(ss);
    for (int z = 0; z < 6; ++z)
        for (int x = 0; x < 4; ++x) {
            MeshInstance m = at(x % 2 ? 1 : 3, {f32(x) * 2.0f - 3.0f, 0.0f, f32(z) * 3.0f - 8.0f});
            if (z == 1) m.tint = {0.2f, 0.9f, 0.2f, 1.0f};
            scene.add(m);
        }
    scene.add(at(4, {0, 0, 0}));
    scene.add(at(4, {1, 0, 0}));
    scene.build();
    REQUIRE(scene.cascades().size() == 3);
    Array<u32> sorted;
    Array<DrawRun> runs;
    u32 instanced_total = 0;
    for (const Cascade& c : scene.cascades()) {
        const Span<const u32> list = scene.cascade_casters().subspan(c.first_caster, c.caster_count);
        scene.caster_runs(list, sorted, runs);
        REQUIRE(sorted.size() == list.size());
        u32 next = 0, sum_in = 0, sum_out = 0;
        for (const DrawRun& r : runs) {
            CHECK(r.first == next);
            next += r.count;
            for (u32 k = 1; k < r.count; ++k) {
                const Scene3D::Draw& a = scene.shadow_draws()[sorted[r.first]];
                const Scene3D::Draw& b = scene.shadow_draws()[sorted[r.first + k]];
                CHECK(a.mesh == b.mesh);
                CHECK(a.submesh == b.submesh);
                CHECK(a.lod == b.lod);
                CHECK_FALSE(a.mesh->skinned());
            }
            if (r.count > 1) instanced_total += r.count;
        }
        CHECK(next == list.size());
        for (u32 i = 0; i < list.size(); ++i) {  // a permutation of the list
            sum_in += list[i] * 31 + 7;
            sum_out += sorted[i] * 31 + 7;
        }
        CHECK(sum_in == sum_out);
        // At most one run per (mesh, submesh, level) besides the skinned casters' single draws.
        u32 multi = 0;
        for (const DrawRun& r : runs) multi += r.count > 1 ? 1u : 0u;
        CHECK(multi <= 4);
    }
    CHECK(instanced_total > 0);

    // With instancing off the lists come back as they are, one draw per run.
    scene.set_instancing({false, 2});
    const Cascade& c0 = scene.cascades()[0];
    const Span<const u32> list = scene.cascade_casters().subspan(c0.first_caster, c0.caster_count);
    scene.caster_runs(list, sorted, runs);
    REQUIRE(sorted.size() == list.size());
    for (u32 i = 0; i < list.size(); ++i) CHECK(sorted[i] == list[i]);
    CHECK(runs.size() == list.size());
}

TEST_CASE("instancing: a group longer than max_count splits into runs of at most max_count") {
    MeshStore store;
    REQUIRE(store.upload(1, 1, box_mesh().span()).has_value());
    Scene3D scene(store, mem::heap());
    InstancingSettings is;
    is.max_count = 3;
    scene.set_instancing(is);
    scene.begin(camera(), {256, 256});
    for (u32 i = 0; i < 7; ++i) scene.add(at(1, {0.0f, 0.0f, -5.0f - f32(i)}));
    scene.add(at(1, {2.0f, 0.0f, -20.0f}));  // eight copies: 3 + 3 + 2
    scene.build();
    check_cover(scene);
    CHECK(scene.stats().runs_instanced == 3);
    CHECK(scene.stats().draws_instanced == 8);
    for (const DrawRun& r : scene.opaque_runs()) CHECK(r.count <= 3);

    // A tail under min_count draws singly.
    Scene3D tail(store, mem::heap());
    tail.set_instancing({true, 2, 3});
    tail.begin(camera(), {256, 256});
    for (u32 i = 0; i < 7; ++i) tail.add(at(1, {0.0f, 0.0f, -5.0f - f32(i)}));  // 3 + 3 + 1
    tail.build();
    check_cover(tail);
    CHECK(tail.stats().runs_instanced == 2);
    CHECK(tail.stats().draws_instanced == 6);
    CHECK(tail.stats().not_instanced_single == 1);

    // The casters split the same way.
    Array<u32> list, sorted;
    Array<DrawRun> runs;
    for (u32 i = 0; i < tail.shadow_draws().size(); ++i) list.push_back(i);
    tail.caster_runs(list.span(), sorted, runs);
    u32 next = 0;
    for (const DrawRun& r : runs) {
        CHECK(r.first == next);
        CHECK(r.count <= 3);
        next += r.count;
    }
    CHECK(next == list.size());
}

TEST_CASE("instancing: alpha-masked casters stay single") {
    MeshStore store;
    REQUIRE(store.upload(5, 1, box_mesh(asset::MaterialFlags::AlphaMask).span()).has_value());
    Scene3D scene(store, mem::heap());
    scene.begin(camera(), {256, 256});
    for (u32 i = 0; i < 4; ++i) scene.add(at(5, {f32(i) - 1.5f, 0.0f, -6.0f}));
    scene.build();
    REQUIRE(scene.shadow_draws().size() == 4);
    Array<u32> list, sorted;
    Array<DrawRun> runs;
    for (u32 i = 0; i < 4; ++i) list.push_back(i);
    scene.caster_runs(list.span(), sorted, runs);
    CHECK(runs.size() == 4);
    for (const DrawRun& r : runs) CHECK(r.count == 1);
}

TEST_CASE("instancing: the singles say whether the mesh is unique, differs by tint, or is a group under min_count") {
    MeshStore store;
    REQUIRE(store.upload(1, 1, box_mesh().span()).has_value());
    REQUIRE(store.upload(2, 1, box_mesh(0, 0.3f).span()).has_value());
    REQUIRE(store.upload(3, 1, box_mesh(0, 0.2f).span()).has_value());
    const auto build = [&](u32 min_count, Scene3D& scene) {
        scene.set_instancing({true, min_count});
        scene.begin(camera(), {256, 256});
        for (int i = 0; i < 4; ++i) scene.add(at(1, {f32(i) - 1.5f, 0.0f, -5.0f}));  // four copies of mesh 1
        MeshInstance red = at(1, {0.0f, 2.0f, -5.0f});  // the same mesh, another tint
        red.tint = {1, 0, 0, 1};
        scene.add(red);
        scene.add(at(2, {0.0f, -2.0f, -5.0f}));  // mesh 2 once
        scene.add(at(3, {1.0f, -2.0f, -5.0f}));  // mesh 3 twice (a pair)
        scene.add(at(3, {2.0f, -2.0f, -5.0f}));
        scene.build();
    };
    Scene3D scene(store, mem::heap());
    build(2, scene);
    check_cover(scene);
    const Scene3D::Stats& st = scene.stats();
    CHECK(st.runs_instanced == 2);  // mesh 1's four, mesh 3's pair
    CHECK(st.draws_instanced == 6);
    CHECK(st.not_instanced_single == 2);
    CHECK(st.not_instanced_unique_mesh == 1);  // mesh 2
    CHECK(st.not_instanced_short == 0);
    // single - unique - short = the red copy: its mesh is shared, its material key is not.

    Scene3D wide(store, mem::heap());
    build(3, wide);  // the pair is now too short to instance
    check_cover(wide);
    CHECK(wide.stats().runs_instanced == 1);
    CHECK(wide.stats().not_instanced_single == 4);
    CHECK(wide.stats().not_instanced_unique_mesh == 1);
    CHECK(wide.stats().not_instanced_short == 2);
}
