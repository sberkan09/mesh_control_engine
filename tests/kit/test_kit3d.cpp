// M-KIT3D: the 3D starter kit without a GPU — the components' reflection, install, the Animator and
// Environment3D systems, StaticInstances3D cells, and Kit3DExtract filling a CPU-side Scene3D
// (meshes, poses, lights, camera interpolation, culling, environment).

#include <doctest/doctest.h>

#include <cmath>

#include <cstdio>
#include <filesystem>
#include <string>

#include "mce/asset/asset_system.h"
#include "mce/asset/container.h"
#include "mce/asset/xxhash.h"
#include "mce/ecs/transform.h"
#include "mce/cook/cook.h"
#include "mce/kit/anim3d.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/memory/frame_slots.h"
#include "mce/refl/type.h"
#include "cook_test_util.h"
#include "mesh_fixture.h"

using namespace mce;
using math::quat;
using math::vec3;

namespace {

bool same_matrix(const math::mat4& a, const math::mat4& b) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (a.c[c][r] != b.c[c][r]) return false;
    return true;
}

constexpr f64 kDt = 1.0 / 60.0;
const asset::Guid kBox{0x3d00000000000001ull, 0x8000000000000001ull};
const asset::Guid kSpinner{0x3d00000000000002ull, 0x8000000000000001ull};

LocalTransform at(vec3 p, quat r = quat::identity()) { return LocalTransform{p, r, {1, 1, 1}}; }

struct Fixture {
    render::MeshStore store;
    World world;
    kit::Kit3DExtract extract;
    u64 tick = 0;

    Fixture() {
        mesh_fixture::Builder box;
        box.add_node(-1);
        mesh_fixture::Box b;
        b.half = {0.5f, 0.5f, 0.5f};
        box.add_box(b);
        const Array<byte> box_blob = box.build();
        REQUIRE(store.upload(1, 1, box_blob.span()).has_value());

        mesh_fixture::Builder spin;
        const u32 root = spin.add_node(-1);
        const u32 child = spin.add_node(static_cast<i32>(root), {2, 0, 0});
        mesh_fixture::Box cb;
        cb.half = {0.1f, 0.1f, 0.1f};
        cb.node = child;
        spin.add_box(cb);
        spin.add_spin_clip(root, 4.0f, "spin");
        spin.add_spin_clip(root, 2.0f, "fast");
        const Array<byte> spin_blob = spin.build();
        REQUIRE(store.upload(2, 1, spin_blob.span()).has_value());

        kit::install_kit3d(world, nullptr);
        extract.init(store, nullptr);
        extract.bind_mesh(kBox, 1);
        extract.bind_mesh(kSpinner, 2);
    }
    void step(u32 n = 1) {
        for (u32 i = 0; i < n; ++i) world.tick(kDt, tick++);
    }
    // A camera at the origin looking down -Z.
    Entity camera(vec3 p = {}) { return world.spawn_with(at(p), kit::Camera3DComponent{}); }
    Entity mesh(asset::Guid g, vec3 p) {
        kit::MeshRef m;
        m.mesh.guid = g;
        return world.spawn_with(at(p), m);
    }
    struct Built {
        render::Scene3D scene;
        explicit Built(const render::MeshStore& s) : scene(s, mem::heap()) {}
    };
};

Array<byte> box_mesh_blob() {
    mesh_fixture::Builder box;
    box.add_node(-1, {0, 0.5f, 0});  // the node lifts the unit box onto y = 0
    mesh_fixture::Box b;
    b.half = {0.5f, 0.5f, 0.5f};
    box.add_box(b);
    return box.build();
}

// A dev cooked tree holding one mesh (manifest + content-addressed file), as the asset tests write.
std::string write_mesh_tree(const char* name, asset::Guid guid, Span<const byte> blob) {
    namespace fs = std::filesystem;
    const fs::path root = fs::path(MCE_TEST_TMP_DIR) / name;
    fs::remove_all(root);
    auto write = [](const fs::path& p, Span<const byte> b) {
        fs::create_directories(p.parent_path());
        std::FILE* f = std::fopen(p.string().c_str(), "wb");
        REQUIRE(f);
        std::fwrite(b.data(), 1, b.size(), f);
        std::fclose(f);
    };
    asset::ContainerWriter w;
    asset::ContainerWriter::ChunkDesc d;
    d.type = asset::ChunkType::Mesh;
    d.version = asset::chunk_version(asset::MESH_VERSION, 0);
    d.guid = guid;
    w.add(d, blob);
    Array<byte> mesh;
    w.finish(mesh);
    const u64 hash = asset::xxh64(mesh.data(), mesh.size());
    const std::string hex = asset::to_hex(guid).c_str();
    char file[64];
    std::snprintf(file, sizeof file, "%016llx.mce", static_cast<unsigned long long>(hash));
    write(root / hex.substr(0, 2) / hex / file, mesh.span());
    asset::ManifestItem item{guid, hash, asset::ChunkType::Mesh, "models/box.glb"};
    Array<byte> mp;
    asset::write_manifest(Span<asset::ManifestItem>(&item, 1), mp);
    asset::ContainerWriter mw;
    asset::ContainerWriter::ChunkDesc md;
    md.type = asset::ChunkType::Manifest;
    mw.add(md, mp.span());
    Array<byte> manifest;
    mw.finish(manifest);
    write(root / "manifest.mce", manifest.span());
    return root.generic_string();
}

}  // namespace

TEST_CASE("kit3d: the components are reflected and install is idempotent") {
    for (const char* name : {"MeshRef", "PointLight3D", "SpotLight3D", "ReflectionProbe3D", "Camera3DComponent", "Animator", "Environment3D",
                             "BoxCollider3D", "MeshCollider3D", "Foliage3D", "Decal3D", "ParticleEmitter3D", "Terrain3D"}) {
        const refl::TypeRecord* t = refl::find_type(StringId(name));
        INFO(name);
        REQUIRE(t != nullptr);
        CHECK(t->is_component());
    }
    World w;
    kit::install_kit3d(w, nullptr);
    w.resource<kit::Environment3D>().exposure = 2.5f;
    kit::install_kit3d(w, nullptr);  // keeps the resource a game already drives
    CHECK(w.resource<kit::Environment3D>().exposure == 2.5f);
    CHECK_FALSE(kit::has_camera_3d(w));
    w.spawn_with(at({}), kit::Camera3DComponent{});
    CHECK(kit::has_camera_3d(w));
}

TEST_CASE("kit3d: the scene's Environment3D becomes the resource at Startup") {
    Fixture f;
    kit::Environment3D authored;
    authored.fog_density = 0.01f;
    authored.exposure = 1.7f;
    authored.sun_intensity = 4.0f;
    f.world.spawn_with(authored);
    f.step();
    CHECK(f.world.resource<kit::Environment3D>().fog_density == doctest::Approx(0.01f));
    CHECK(f.world.resource<kit::Environment3D>().exposure == doctest::Approx(1.7f));

    // The game drives the resource; the extract hands it to the scene.
    f.world.resource<kit::Environment3D>().sun_intensity = 0.5f;
    f.world.resource<kit::Environment3D>().shadow_cascades = 9;  // clamped
    f.world.resource<kit::Environment3D>().ssao = 1;
    f.world.resource<kit::Environment3D>().ssao_radius = 0.9f;
    f.camera();
    render::Camera3D cam;
    REQUIRE(f.extract.camera(f.world, 1.0f, cam));
    render::Scene3D scene(f.store, mem::heap());
    scene.begin(cam, {320, 180});
    f.extract.extract(f.world, scene, 1.0f);
    CHECK(scene.light().intensity == doctest::Approx(0.5f));
    CHECK(scene.fog().density == doctest::Approx(0.01f));
    CHECK(scene.exposure() == doctest::Approx(1.7f));
    CHECK(scene.shadow_settings().cascades == render::kMaxCascades);
    CHECK(scene.ambient_occlusion().enabled);
    CHECK(scene.ambient_occlusion().radius == doctest::Approx(0.9f));
}

TEST_CASE("kit3d: MeshRefs, lights and visibility fill the scene") {
    Fixture f;
    f.camera();
    f.mesh(kBox, {0, 0, -5});
    f.mesh(kBox, {0, 0, 5});  // behind the camera: submitted, culled by build()
    const Entity hidden = f.mesh(kBox, {1, 0, -5});
    f.world.try_get_mut<kit::MeshRef>(hidden)->visible = 0;
    const Entity culled = f.mesh(kBox, {0, 0, 50});
    f.world.try_get_mut<kit::MeshRef>(culled)->cull_radius = 1.0f;  // cull sphere behind the camera: never submitted
    f.mesh(asset::Guid{0x1, 0x2}, {0, 0, -5});  // unbound GUID, no asset system: skipped
    kit::PointLight3D lamp;
    lamp.offset = {0, 1, 0};
    lamp.intensity = 2.0f;
    f.world.spawn_with(at({0, 0, -4}), lamp);
    kit::PointLight3D off;
    off.enabled = 0;
    f.world.spawn_with(at({0, 0, -4}), off);
    kit::SpotLight3D spot;
    spot.outer_deg = 40.0f;
    spot.inner_deg = 60.0f;  // clamped to outer
    spot.cast_shadows = 1;
    f.world.spawn_with(at({0, 2, -6}), spot);
    kit::ReflectionProbe3D probe;
    probe.offset = {0, 1, 0};
    probe.half_extents = {4, 2, 3};
    probe.version = 7;
    f.world.spawn_with(at({1, 0, -5}), probe);
    probe.enabled = 0;
    f.world.spawn_with(at({1, 0, -5}), probe);
    f.step();

    render::Camera3D cam;
    REQUIRE(f.extract.camera(f.world, 1.0f, cam));
    render::Scene3D scene(f.store, mem::heap());
    scene.begin(cam, {320, 180});
    f.extract.extract(f.world, scene, 1.0f);
    scene.build();
    CHECK(f.extract.stats().meshes == 2);
    CHECK(f.extract.stats().point_lights == 1);
    CHECK(f.extract.stats().spot_lights == 1);
    CHECK(scene.stats().instances == 2);
    CHECK(scene.stats().visible == 1);
    REQUIRE(scene.point_lights().size() == 2);
    CHECK(scene.point_lights()[0].position.y == doctest::Approx(1.0f));
    CHECK(scene.point_lights()[0].spot_outer == 0.0f);
    CHECK(scene.point_lights()[1].spot_outer == doctest::Approx(math::radians(40.0f)));
    CHECK(scene.point_lights()[1].spot_inner == doctest::Approx(math::radians(40.0f)));
    CHECK_FALSE(scene.point_lights()[0].cast_shadows);  // a light casts shadows only when it asks
    CHECK(scene.point_lights()[1].cast_shadows);
    CHECK(f.extract.stats().reflection_probes == 1);
    REQUIRE(scene.reflection_probes().size() == 1);
    const render::ReflectionProbe& rp = scene.reflection_probes()[0];
    CHECK(rp.position.y == doctest::Approx(1.0f));
    CHECK(rp.bounds.min.x == doctest::Approx(-3.0f));
    CHECK(rp.bounds.max.z == doctest::Approx(-2.0f));
    CHECK(rp.version == 7);
    CHECK(rp.baked == ~0u);  // no bake: captured at runtime
}

TEST_CASE("kit3d: the camera and interpolated meshes draw between the last two ticks") {
    Fixture f;
    const Entity cam_e = f.camera({0, 0, 0});
    const Entity box = f.mesh(kBox, {0, 0, -5});
    f.world.try_get_mut<kit::MeshRef>(box)->interpolate = 1;
    f.step();
    render::Camera3D cam;
    REQUIRE(f.extract.camera(f.world, 0.5f, cam));  // first sight: no history, the current pose
    CHECK(cam.position.x == doctest::Approx(0.0f));
    {
        render::Scene3D scene(f.store, mem::heap());
        scene.begin(cam, {64, 64});
        f.extract.extract(f.world, scene, 0.5f);
    }

    f.world.try_get_mut<LocalTransform>(cam_e)->position = {2, 0, 0};
    f.world.try_get_mut<LocalTransform>(box)->position = {4, 0, -5};
    f.step();
    REQUIRE(f.extract.camera(f.world, 0.5f, cam));
    CHECK(cam.position.x == doctest::Approx(1.0f));
    render::Scene3D scene(f.store, mem::heap());
    scene.begin(cam, {64, 64});
    f.extract.extract(f.world, scene, 0.5f);
    REQUIRE(scene.instances().size() == 1);
    CHECK(math::transform_point(scene.instances()[0].world, {0, 0, 0}).x == doctest::Approx(2.0f));

    // A second frame inside the same tick keeps the same history; alpha 1 is the newest pose.
    REQUIRE(f.extract.camera(f.world, 1.0f, cam));
    CHECK(cam.position.x == doctest::Approx(2.0f));
}

TEST_CASE("kit3d: Animator advances, cross-fades and poses the mesh") {
    Fixture f;
    f.camera();
    const Entity e = f.mesh(kSpinner, {0, 0, -5});
    kit::Animator a;
    a.clip = "spin";
    a.fade = 0.5f;
    f.world.add(e, a);
    f.step();  // the first tick starts the clip at 0
    CHECK(f.world.try_get<kit::Animator>(e)->playing == StringId("spin").value);
    CHECK(f.world.try_get<kit::Animator>(e)->time == 0.0f);
    CHECK(f.world.try_get<kit::Animator>(e)->blend == 1.0f);
    f.step(60);
    CHECK(f.world.try_get<kit::Animator>(e)->time == doctest::Approx(1.0f));

    // A quarter turn of "spin" at t = 1: the child (2 m along +X) swings to -Z.
    render::Camera3D cam;
    REQUIRE(f.extract.camera(f.world, 1.0f, cam));
    {
        render::Scene3D scene(f.store, mem::heap());
        scene.begin(cam, {64, 64});
        f.extract.extract(f.world, scene, 1.0f);
        REQUIRE(scene.instances().size() == 1);
        const Span<const math::mat4> pose = scene.instances()[0].pose;
        REQUIRE(pose.size() == 2);
        const vec3 p = math::transform_point(pose[1], {0, 0, 0});
        CHECK(p.x == doctest::Approx(0.0f).epsilon(1e-3));
        CHECK(p.z == doctest::Approx(-2.0f).epsilon(1e-3));
        CHECK(f.extract.stats().animated == 1);
    }

    f.world.try_get_mut<kit::Animator>(e)->clip = "fast";
    f.step();
    const kit::Animator& mid = *f.world.try_get<kit::Animator>(e);
    CHECK(mid.previous == StringId("spin").value);
    CHECK(mid.time == 0.0f);
    CHECK(mid.blend == 0.0f);
    f.step(15);
    CHECK(f.world.try_get<kit::Animator>(e)->blend == doctest::Approx(0.5f));
    f.step(15);
    CHECK(f.world.try_get<kit::Animator>(e)->blend == 1.0f);
    CHECK(f.world.try_get<kit::Animator>(e)->previous == 0);

    // An unknown clip: the rest pose (no pose span).
    f.world.try_get_mut<kit::Animator>(e)->clip = "nope";
    f.step();
    render::Scene3D scene(f.store, mem::heap());
    scene.begin(cam, {64, 64});
    f.extract.extract(f.world, scene, 1.0f);
    REQUIRE(scene.instances().size() == 1);
    CHECK(scene.instances()[0].pose.empty());
}

TEST_CASE("kit3d: StaticInstances3D cells cull and thin by distance") {
    Fixture f;
    f.camera();
    kit::StaticInstances3D& si = f.world.resource<kit::StaticInstances3D>();
    const u32 slot = si.mesh_slot(kBox);
    CHECK(si.mesh_slot(kBox) == slot);
    CHECK(si.mesh_slot(asset::Guid{}) == ~0u);
    for (u32 i = 0; i < 40; ++i) {
        kit::StaticInstance3D s;
        const u32 row = i / 4, column = i % 4;
        s.position = {f32(column) * 2.0f, 0.0f, -10.0f - f32(row) * 20.0f};  // 10 rows going away
        s.mesh = slot;
        s.thin_distance = 60.0f;
        s.max_distance = 150.0f;
        si.instances.push_back(s);
    }
    kit::StaticInstance3D behind;
    behind.position = {0, 0, 40};
    behind.mesh = slot;
    si.instances.push_back(behind);
    si.build_cells(16.0f, 1.0f);
    u32 total = 0;
    for (const kit::StaticCell3D& c : si.cells) total += c.count;
    CHECK(total == 41);
    CHECK(si.cells.size() >= 10);

    render::Camera3D cam;
    REQUIRE(f.extract.camera(f.world, 1.0f, cam));
    render::Scene3D scene(f.store, mem::heap());
    scene.begin(cam, {320, 180});
    f.extract.extract(f.world, scene, 1.0f);
    // Rows at 10..50 m: all 4; 70..150 m (cell distance): every other; past 150 m: none; behind: culled.
    CHECK(f.extract.stats().instances > 12);
    CHECK(f.extract.stats().instances < 40);
    CHECK(f.extract.stats().cells_visible < si.cells.size());
}

TEST_CASE("kit3d: mesh_triangles places every submesh by its rest-pose node") {
    const Array<byte> blob = box_mesh_blob();
    Array<vec3> verts;
    Array<u32> indices;
    REQUIRE(kit::mesh_triangles(blob.span(), math::scaling({2, 2, 2}), verts, indices));
    CHECK(indices.size() == 36);
    f32 lo = 1e9f, hi = -1e9f;
    for (const vec3& v : verts) {
        lo = math::min(lo, v.y);
        hi = math::max(hi, v.y);
    }
    CHECK(lo == doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(hi == doctest::Approx(2.0f).epsilon(1e-3));
    for (u32 i : indices) CHECK(i < verts.size());
    const byte junk[8] = {};
    CHECK_FALSE(kit::mesh_triangles(Span<const byte>(junk, 8), math::mat4::identity(), verts, indices));
}

TEST_CASE("kit3d: box and mesh colliders become static bodies at Startup") {
    const Array<byte> blob = box_mesh_blob();
    const std::string root = write_mesh_tree("kit3d_colliders", kBox, blob.span());
    asset::AssetSystem assets;
    const StringView roots[1] = {StringView(root.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)).has_value());

    World w;
    kit::install_kit3d(w, &assets);
    kit::BoxCollider3D box;
    box.half = {1, 1, 1};
    LocalTransform t = at({0, 0, 0});
    t.scale = {2, 2, 2};
    w.spawn_with(t, box);
    kit::BoxCollider3D ramp;
    ramp.offset = {0, 0, 0};
    ramp.pitch_deg = 30.0f;
    w.spawn_with(at({-20, 0, 0}), ramp);
    kit::MeshCollider3D mc;
    mc.mesh.guid = kBox;
    LocalTransform mt = at({10, 1, 0});
    mt.scale = {3, 3, 3};
    w.spawn_with(mt, mc);
    kit::MeshCollider3D missing;
    missing.mesh.guid = asset::Guid{0x99, 0x99};
    w.spawn_with(at({30, 0, 0}), missing);  // skipped with a warning
    w.tick(kDt, 0);

    kit::Physics3D& phys = w.resource<kit::Physics3D>();
    CHECK(phys.colliders == 3);
    const auto hit_box = phys.world.raycast({0, 10, 0}, {0, -1, 0}, 20.0f);
    REQUIRE(hit_box.has_value());
    CHECK(hit_box->point.y == doctest::Approx(2.0f).epsilon(1e-3));
    const auto hit_mesh = phys.world.raycast({10, 10, 0}, {0, -1, 0}, 20.0f);
    REQUIRE(hit_mesh.has_value());
    CHECK(hit_mesh->point.y == doctest::Approx(4.0f).epsilon(1e-2));  // 1 + the 3 m tall box
    // The kit steps the world (auto_step); a game can turn that off and step it itself.
    phys.auto_step = 0;
    w.tick(kDt, 1);
}

TEST_CASE("kit3d: a game's own Physics3D config survives install") {
    World w;
    physics::PhysicsConfig3D cfg;
    cfg.max_bodies = 77;
    w.set_resource(kit::Physics3D{physics::World3D(cfg), 0, 0});
    kit::install_kit3d(w, nullptr);
    CHECK(w.resource<kit::Physics3D>().world.config().max_bodies == 77);
    CHECK(w.resource<kit::Physics3D>().auto_step == 0);
}

TEST_CASE("kit3d: an AnimGraph drives the Animator: blend by parameter, transitions, events, one-shots; AttachToBone follows") {
    // The spinner, with a named child, in a cooked tree next to the cooked graph.
    mesh_fixture::Builder spin;
    const u32 root = spin.add_node(-1);
    const u32 tip = spin.add_node(static_cast<i32>(root), {2, 0, 0}, "tip");
    mesh_fixture::Box cb;
    cb.half = {0.1f, 0.1f, 0.1f};
    cb.node = tip;
    spin.add_box(cb);
    spin.add_spin_clip(root, 4.0f, "spin");
    spin.add_spin_clip(root, 2.0f, "fast");
    const Array<byte> blob = spin.build();
    const std::string mesh_root = write_mesh_tree("kit3d_animgraph_mesh", kSpinner, blob.span());

    const std::string src = cook_test::fresh_dir("kit3d_animgraph");
    cook_test::write_file(src + "/assets/spinner.mcanim", R"({ "format": "mce-animgraph", "version": 1,
        "guid": "9a270000-0000-4000-8000-000000000051", "name": "spinner", "params": ["speed", "attack"],
        "states": [
            { "name": "move", "blend": { "param": "speed", "points": [ { "clip": "spin", "at": 0 }, { "clip": "fast", "at": 1 } ] },
              "events": [ { "time": 1.0, "name": "tick" } ] },
            { "name": "attack", "clip": "fast", "loop": false, "events": [ { "time": 0.5, "name": "hit" } ] } ],
        "transitions": [
            { "from": "*", "to": "attack", "when": [ { "param": "attack", "op": ">", "value": 0.5 } ], "fade": 0.25 },
            { "from": "attack", "to": "move", "on_finish": true, "fade": 0 } ] })");
    cook::CookService svc;
    const cook::CookReport r = svc.cook(StringView(src.c_str()), cook::CookOptions{});
    REQUIRE(r.ok());
    asset::AssetSystem assets;
    const std::string cooked = src + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str()), StringView(mesh_root.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 2)));

    Fixture f;
    kit::install_kit3d(f.world, &assets);
    f.camera();
    const Entity e = f.mesh(kSpinner, {0, 0, -5});
    kit::Animator a;
    a.graph.guid = asset::parse_guid("9a270000-0000-4000-8000-000000000051").value();
    f.world.add(e, a);
    kit::AttachToBone at;
    at.target = e;
    at.bone = "tip";
    at.offset = {0, 1, 0};
    const Entity sword = f.world.spawn_with(LocalTransform{}, at);
    const auto anim = [&]() -> const kit::Animator& { return *f.world.try_get<kit::Animator>(e); };

    f.step();  // enters the start state
    CHECK(anim().state.view() == StringView("move"));
    CHECK(anim().playing == StringId("spin").value);
    CHECK(anim().blend_clip == 0);
    // AttachToBone: the tip at rest is 2 m along +X of the spinner, the offset 1 m above it.
    CHECK(f.world.try_get<kit::AttachToBone>(sword)->found == 1);
    CHECK(f.world.try_get<LocalTransform>(sword)->position.x == doctest::Approx(2.0f).epsilon(1e-3));
    CHECK(f.world.try_get<LocalTransform>(sword)->position.y == doctest::Approx(1.0f).epsilon(1e-3));
    CHECK(f.world.try_get<LocalTransform>(sword)->position.z == doctest::Approx(-5.0f).epsilon(1e-3));

    // 1 s into "spin" (a quarter turn): the "tick" event fires and the tip has swung to -Z.
    u32 ticks = 0;
    for (u32 i = 0; i < 61; ++i) {
        f.step();
        ticks += static_cast<u32>(EventAccess(f.world).read<kit::AnimEvent3D>().size());
    }
    CHECK(ticks == 1);
    CHECK(anim().events_fired == 1);
    CHECK(anim().last_event.view() == StringView("tick"));
    const LocalTransform& st = *f.world.try_get<LocalTransform>(sword);
    CHECK(st.position.x == doctest::Approx(0.0f).epsilon(0.05));
    CHECK(st.position.z == doctest::Approx(-7.0f).epsilon(0.05));

    // A parameter between the points blends the two clips.
    f.world.try_get_mut<kit::Animator>(e)->params.x = 0.25f;
    f.step();
    CHECK(anim().blend_clip == StringId("fast").value);
    CHECK(anim().blend_weight == doctest::Approx(0.25f));
    {
        render::Camera3D cam;
        REQUIRE(f.extract.camera(f.world, 1.0f, cam));
        render::Scene3D scene(f.store, mem::heap());
        scene.begin(cam, {64, 64});
        f.extract.extract(f.world, scene, 1.0f);
        CHECK(f.extract.stats().animated == 1);
    }

    // The attack parameter fires the any-state transition, cross-fading over its own fade.
    f.world.try_get_mut<kit::Animator>(e)->params.y = 1.0f;
    f.step();
    CHECK(anim().state.view() == StringView("attack"));
    CHECK(anim().clip.view() == StringView("fast"));
    CHECK(anim().previous == StringId("spin").value);
    CHECK(anim().blend == 0.0f);
    CHECK(anim().fade == doctest::Approx(0.25f));
    f.world.try_get_mut<kit::Animator>(e)->params.y = 0.0f;
    f.step(31);
    CHECK(anim().last_event.view() == StringView("hit"));
    CHECK(anim().blend == 1.0f);

    // The one-shot reaches its end (2 s) and the on_finish transition returns to "move".
    bool finished = false;
    for (u32 i = 0; i < 120 && !finished; ++i) {
        f.step();
        finished = anim().state.view() == StringView("move");
    }
    CHECK(finished);
    CHECK(anim().time == 0.0f);

    // restart returns to the start state; without a graph a one-shot clip holds and finishes.
    f.world.try_get_mut<kit::Animator>(e)->graph.guid = asset::Guid{};
    f.world.try_get_mut<kit::Animator>(e)->clip = "fast";
    f.world.try_get_mut<kit::Animator>(e)->loop = false;
    f.step(1 + 150);
    CHECK(anim().finished);
    CHECK(anim().time < 2.0f);
    CHECK(anim().time > 1.99f);
    f.world.try_get_mut<kit::Animator>(e)->restart = true;
    f.step();
    CHECK_FALSE(anim().finished);
    CHECK(anim().time == 0.0f);
}

TEST_CASE("kit3d: MeshRef.material replaces the mesh's material; the quality tier caps shadows and post (M-DATA-REACH 8)") {
    const std::string src = cook_test::fresh_dir("kit3d_material");
    cook_test::write_file(src + "/assets/red.mcmat", R"({ "format": "mce-material", "version": 1,
        "guid": "9a270000-0000-4000-8000-000000000061", "base_color": [1, 0, 0, 1], "roughness": 0.3 })");
    cook::CookService svc;
    REQUIRE(svc.cook(StringView(src.c_str()), cook::CookOptions{}).ok());
    asset::AssetSystem assets;
    const std::string cooked = src + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));

    Fixture f;
    f.extract.reset(&assets);
    f.extract.bind_mesh(kBox, 1);
    f.camera();
    const Entity red = f.mesh(kBox, {0, 0, -5});
    f.world.try_get_mut<kit::MeshRef>(red)->material.guid = asset::parse_guid("9a270000-0000-4000-8000-000000000061").value();
    f.mesh(kBox, {2, 0, -5});
    f.step();

    render::Camera3D cam;
    REQUIRE(f.extract.camera(f.world, 1.0f, cam));
    {
        render::Scene3D scene(f.store, mem::heap());
        scene.begin(cam, {64, 64});
        f.extract.extract(f.world, scene, 1.0f);
        REQUIRE(scene.instances().size() == 2);
        const render::MaterialOverride* m = scene.instances()[0].material;
        REQUIRE(m != nullptr);
        CHECK(m->params.base_color[0] == 1.0f);
        CHECK(m->params.base_color[1] == 0.0f);
        CHECK(m->params.roughness == doctest::Approx(0.3f));
        CHECK(m->albedo_key == ~0u);
        CHECK(scene.instances()[1].material == nullptr);
    }

    // r.quality low: one cascade at 1024, no bloom.
    f.extract.set_quality(render::quality_settings(render::QualityTier::Low));
    render::Scene3D scene(f.store, mem::heap());
    scene.begin(cam, {64, 64});
    f.extract.extract(f.world, scene, 1.0f);
    CHECK(scene.shadow_settings().cascades == 1);
    CHECK(scene.shadow_settings().resolution == 1024);
    CHECK_FALSE(scene.post().bloom);
}

TEST_CASE("kit3d: Foliage3D, Decal3D and ParticleEmitter3D become a batch, decals and emitters (M-VISUAL 9)") {
    Fixture f;
    f.camera();
    kit::Foliage3D grass;
    grass.mesh.guid = kBox;
    grass.count = 50;
    grass.seed = 3;
    grass.half_extents = {4, 2};
    grass.wind_amplitude = 0.2f;
    const Entity field = f.world.spawn_with(at({0, 0, -10}), grass);
    kit::Foliage3D missing = grass;  // unbound mesh: skipped
    missing.mesh.guid = asset::Guid{0x1, 0x2};
    f.world.spawn_with(at({0, 0, -10}), missing);
    kit::Decal3D decal;
    decal.size = {2, 1, 3};
    decal.color = {1, 0, 0, 1};
    f.world.spawn_with(at({0, 0, -6}), decal);
    decal.enabled = 0;
    f.world.spawn_with(at({0, 0, -6}), decal);
    kit::ParticleEmitter3D sparks;
    sparks.offset = {0, 1, 0};
    sparks.direction = {0, 0, -2};
    sparks.burst = 30;
    const Entity emitter = f.world.spawn_with(at({2, 0, -6}), sparks);
    f.step();

    auto frame = [&](render::Scene3D& scene) {
        render::Camera3D cam;
        REQUIRE(f.extract.camera(f.world, 1.0f, cam));
        scene.begin(cam, {320, 180});
        f.extract.extract(f.world, scene, 1.0f);
        scene.build();
    };
    f.extract.set_frame_dt(0.25f);
    render::Scene3D scene(f.store, mem::heap());
    frame(scene);
    CHECK(f.extract.stats().foliage == 1);
    CHECK(f.extract.stats().foliage_instances == 50);
    CHECK(f.extract.stats().missing == 1);
    CHECK(f.extract.stats().decals == 1);
    CHECK(f.extract.stats().particle_emitters == 1);
    REQUIRE(scene.batches().size() == 1);
    const render::InstanceBatch& b = scene.batches()[0];
    CHECK(b.mesh == 1);
    CHECK(b.transforms.size() == 50);
    CHECK(b.wind_amplitude == doctest::Approx(0.2f));
    for (const math::mat4& m : b.transforms) {
        CHECK(std::fabs(m.c[3][0]) <= 4.0f);
        CHECK(m.c[3][2] <= -8.0f);
        CHECK(m.c[3][2] >= -12.0f);
    }
    const u32 version = b.version;
    const math::mat4 first = b.transforms[0];
    REQUIRE(scene.decals().size() == 1);
    CHECK(scene.decals()[0].world.c[0][0] == doctest::Approx(2.0f));
    CHECK(scene.decals()[0].world.c[2][2] == doctest::Approx(3.0f));
    CHECK(scene.decals()[0].world.c[3][2] == doctest::Approx(-6.0f));
    CHECK(scene.decals()[0].texture == ~0u);
    REQUIRE(scene.particle_emitters().size() == 1);
    const render::ParticleEmitter& em = scene.particle_emitters()[0];
    CHECK(em.position.y == doctest::Approx(1.0f));
    CHECK(em.direction.z == doctest::Approx(-1.0f));
    CHECK(em.burst == 30);  // a new emitter bursts once
    CHECK(scene.time() == doctest::Approx(0.25f));
    CHECK(scene.dt() == doctest::Approx(0.25f));

    // Unchanged: the same scatter and version, no burst, the clock runs on.
    render::Scene3D again(f.store, mem::heap());
    frame(again);
    CHECK(again.batches()[0].version == version);
    CHECK(same_matrix(again.batches()[0].transforms[0], first));
    CHECK(again.particle_emitters()[0].burst == 0);
    CHECK(again.time() == doctest::Approx(0.5f));
    // A new seed re-scatters under a new version; a bumped burst_version bursts again.
    f.world.try_get_mut<kit::Foliage3D>(field)->seed = 4;
    f.world.try_get_mut<kit::ParticleEmitter3D>(emitter)->burst_version = 1;
    render::Scene3D third(f.store, mem::heap());
    frame(third);
    CHECK(third.batches()[0].version != version);
    CHECK(!same_matrix(third.batches()[0].transforms[0], first));
    CHECK(third.particle_emitters()[0].burst == 30);
}

TEST_CASE("kit3d: Terrain3D draws its LOD nodes, uploads them once and drops them with the entity (M-TERRAIN 1)") {
    const std::string src = cook_test::fresh_dir("kit3d_terrain");
    // A 129-sample, 128 m terrain: a ridge along x (heights from 0 to 32 m) so far nodes coarsen.
    std::string bytes;
    for (u32 z = 0; z < 129; ++z)
        for (u32 x = 0; x < 129; ++x) {
            const f32 h = 0.5f + 0.5f * std::sin(static_cast<f32>(x) * 0.2f) * std::cos(static_cast<f32>(z) * 0.15f);
            cook_test::put16le(bytes, static_cast<u16>(h * 65535.0f));
        }
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    for (size_t i = 0; i + 2 < bytes.size(); i += 3) {
        const u32 v = (u32(u8(bytes[i])) << 16) | (u32(u8(bytes[i + 1])) << 8) | u8(bytes[i + 2]);
        for (int s = 18; s >= 0; s -= 6) b64.push_back(k[(v >> s) & 63]);
    }
    if (bytes.size() % 3 == 2) {
        const size_t i = bytes.size() - 2;
        const u32 v = (u32(u8(bytes[i])) << 16) | (u32(u8(bytes[i + 1])) << 8);
        b64 += {k[(v >> 18) & 63], k[(v >> 12) & 63], k[(v >> 6) & 63], '='};
    }
    cook_test::write_file(src + "/assets/ground.mcterrain", R"({ "format": "mce-terrain", "version": 1,
        "guid": "7e7a0000-0000-4000-8000-0000000000b1", "resolution": 129, "chunk": 16, "size": [128, 128],
        "height_range": [0, 32], "heights": ")" + b64 + "\" }");
    cook_test::write_file(src + "/assets/grass.mcmat", R"({ "format": "mce-material", "version": 1,
        "guid": "7e7a0000-0000-4000-8000-0000000000b2", "base_color": [0.2, 0.5, 0.1, 1] })");
    cook::CookService svc;
    REQUIRE(svc.cook(StringView(src.c_str()), cook::CookOptions{}).ok());
    asset::AssetSystem assets;
    const std::string cooked = src + "/cooked";
    const StringView roots[] = {StringView(cooked.c_str())};
    REQUIRE(assets.mount(Span<const StringView>(roots, 1)));

    Fixture f;
    f.extract.reset(&assets);
    f.world.spawn_with(at({-60.0f, 34.0f, 60.0f}, math::quat_from_axis_angle({0, 1, 0}, math::radians(-45.0f))), kit::Camera3DComponent{});
    kit::Terrain3D t;
    t.terrain.guid = asset::parse_guid("7e7a0000-0000-4000-8000-0000000000b1").value();
    t.material.guid = asset::parse_guid("7e7a0000-0000-4000-8000-0000000000b2").value();
    t.pixel_error = 1.0f;
    const Entity ground = f.world.spawn_with(at({0.0f, 0.0f, 0.0f}), t);
    f.step();

    auto frame = [&](render::Scene3D& scene) {
        render::Camera3D cam;
        REQUIRE(f.extract.camera(f.world, 1.0f, cam));
        scene.begin(cam, {640, 360});
        f.extract.extract(f.world, scene, 1.0f);
    };
    const u32 meshes_before = f.store.live_meshes();
    u32 drawn = 0;
    {
        render::Scene3D scene(f.store, mem::heap());
        frame(scene);
        const kit::Kit3DStats& s = f.extract.stats();
        CHECK(s.terrains == 1);
        CHECK(s.terrain_nodes > 1);
        CHECK(s.terrain_uploads == s.terrain_nodes);
        CHECK(s.terrain_resident == s.terrain_nodes);
        CHECK(s.terrain_triangles > 0);
        drawn = s.terrain_nodes;
        REQUIRE(scene.instances().size() == drawn);
        for (const render::MeshInstance& inst : scene.instances()) {
            CHECK(inst.mesh >= render::MeshStore::kFirstLocalKey);
            REQUIRE(f.store.get(inst.mesh) != nullptr);
            REQUIRE(inst.material != nullptr);
            CHECK(inst.material->params.base_color[1] == doctest::Approx(0.5f));
            CHECK(inst.prev_world != nullptr);
        }
        CHECK(f.store.live_meshes() == meshes_before + drawn);
    }
    {  // the same view: nothing new uploaded
        render::Scene3D scene(f.store, mem::heap());
        frame(scene);
        CHECK(f.extract.stats().terrain_nodes == drawn);
        CHECK(f.extract.stats().terrain_uploads == 0);
    }
    {  // a coarser error bound draws fewer nodes; the finer ones stay resident for now
        f.world.try_get_mut<kit::Terrain3D>(ground)->pixel_error = 32.0f;
        render::Scene3D scene(f.store, mem::heap());
        frame(scene);
        CHECK(f.extract.stats().terrain_nodes < drawn);
        CHECK(f.extract.stats().terrain_resident >= drawn);
    }
    {  // a new UV tiling rebuilds every node mesh
        f.world.try_get_mut<kit::Terrain3D>(ground)->uv_tile = 4.0f;
        render::Scene3D scene(f.store, mem::heap());
        frame(scene);
        CHECK(f.extract.stats().terrain_uploads == f.extract.stats().terrain_nodes);
        CHECK(f.store.live_meshes() == meshes_before + f.extract.stats().terrain_nodes);
    }
    // Gone with its entity.
    f.world.destroy(ground);
    {
        render::Scene3D scene(f.store, mem::heap());
        frame(scene);
        CHECK(f.extract.stats().terrains == 0);
        CHECK(f.store.live_meshes() == meshes_before);
    }

    // A const store (no uploads): the terrain counts as missing instead of drawing.
    kit::Kit3DExtract read_only;
    const render::MeshStore& cstore = f.store;
    read_only.init(cstore, &assets);
    f.world.spawn_with(at({0.0f, 0.0f, 0.0f}), t);
    render::Scene3D scene(f.store, mem::heap());
    render::Camera3D cam;
    REQUIRE(read_only.camera(f.world, 1.0f, cam));
    scene.begin(cam, {640, 360});
    read_only.extract(f.world, scene, 1.0f);
    CHECK(read_only.stats().terrain_nodes == 0);
    CHECK(read_only.stats().missing > 0);
}
