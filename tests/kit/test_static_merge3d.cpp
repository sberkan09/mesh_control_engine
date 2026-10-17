// StaticMerge3D (M-VISUAL3 5b) through Kit3DExtract, without a GPU: the rigid MeshRef children of an entity merge into
// one mesh per cell and material and draw in its place; animated, blended and moving-flagged children stay separate;
// moving a child (or bumping `version`) re-merges and retires the old mesh; removing the component, disabling it or
// switching the extract's merging off restores the per-child draws.

#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "mce/asset/asset_system.h"
#include "mce/asset/container.h"
#include "mce/asset/formats.h"
#include "mce/asset/xxhash.h"
#include "mce/cook/cook.h"
#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/kit3d_extract.h"
#include "mce/kit/static_merge3d.h"
#include "mce/memory/frame_slots.h"
#include "mce/refl/type.h"
#include "cook_test_util.h"
#include "mesh_fixture.h"

using namespace mce;
using math::quat;
using math::vec3;

namespace {

// The kit sits around x = 8 so that it stays inside one cell of 16 or 32 m (cells are anchored at the origin).
const vec3 kOrigin{8.0f, 0.0f, 0.0f};
const asset::Guid kRedA{0x5b00000000000001ull, 0x8000000000000001ull};
const asset::Guid kRedB{0x5b00000000000002ull, 0x8000000000000001ull};
const asset::Guid kGreen{0x5b00000000000003ull, 0x8000000000000001ull};
const asset::Guid kGlass{0x5b00000000000004ull, 0x8000000000000001ull};
const asset::Guid kSpin{0x5b00000000000005ull, 0x8000000000000001ull};

struct Piece {
    asset::Guid guid;
    Array<byte> blob;
};

Array<byte> box_blob(vec3 half, f32 r, f32 g, f32 b, u32 flags = 0, bool spin = false) {
    mesh_fixture::Builder bld;
    const u32 root = bld.add_node(-1);
    mesh_fixture::Box box;
    box.center = {0, half.y, 0};
    box.half = half;
    box.color[0] = r;
    box.color[1] = g;
    box.color[2] = b;
    box.flags = flags;
    bld.add_box(box);
    if (spin) bld.add_spin_clip(root, 4.0f, "spin");
    return bld.build();
}

// A dev cooked tree of `pieces` (manifest + one content-addressed file each), as kit_mesh_tree.h writes for one mesh.
std::string write_tree(const char* name, const Array<Piece>& pieces) {
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
    Array<asset::ManifestItem> items;
    for (const Piece& p : pieces) {
        asset::ContainerWriter w;
        asset::ContainerWriter::ChunkDesc d;
        d.type = asset::ChunkType::Mesh;
        d.version = asset::chunk_version(asset::MESH_VERSION, 0);
        d.guid = p.guid;
        w.add(d, p.blob.span());
        Array<byte> mesh;
        w.finish(mesh);
        const u64 hash = asset::xxh64(mesh.data(), mesh.size());
        const std::string hex = asset::to_hex(p.guid).c_str();
        char file[64];
        std::snprintf(file, sizeof file, "%016llx.mce", static_cast<unsigned long long>(hash));
        write(root / hex.substr(0, 2) / hex / file, mesh.span());
        items.push_back(asset::ManifestItem{p.guid, hash, asset::ChunkType::Mesh, "models/piece.glb"});
    }
    Array<byte> mp;
    asset::write_manifest(Span<asset::ManifestItem>(items.data(), items.size()), mp);
    asset::ContainerWriter mw;
    asset::ContainerWriter::ChunkDesc md;
    md.type = asset::ChunkType::Manifest;
    mw.add(md, mp.span());
    Array<byte> manifest;
    mw.finish(manifest);
    write(root / "manifest.mce", manifest.span());
    return root.generic_string();
}

struct Fixture {
    render::MeshStore store;
    asset::AssetSystem assets;
    World world;
    kit::Kit3DExtract extract;
    u64 tick = 0;
    Entity root;

    explicit Fixture(const char* name, bool writable = true, const std::string& extra_root = {}) {
        Array<Piece> pieces;
        pieces.push_back({kRedA, box_blob({0.5f, 0.5f, 0.5f}, 0.8f, 0.2f, 0.2f)});
        pieces.push_back({kRedB, box_blob({0.25f, 1.0f, 0.25f}, 0.8f, 0.2f, 0.2f)});
        pieces.push_back({kGreen, box_blob({0.5f, 0.1f, 0.5f}, 0.2f, 0.8f, 0.2f)});
        pieces.push_back({kGlass, box_blob({0.5f, 0.5f, 0.1f}, 0.5f, 0.5f, 0.9f, asset::MaterialFlags::AlphaBlend)});
        pieces.push_back({kSpin, box_blob({0.3f, 0.3f, 0.3f}, 0.8f, 0.2f, 0.2f, 0, true)});
        const std::string tree = write_tree(name, pieces);
        const StringView roots[] = {StringView(tree.c_str()), StringView(extra_root.c_str())};
        REQUIRE(assets.mount(Span<const StringView>(roots, extra_root.empty() ? 1 : 2)));
        // No upload sink here: the store gets each mesh under its asset key by hand.
        for (const Piece& p : pieces) REQUIRE(store.upload(assets.find(p.guid), 1, p.blob.span()).has_value());
        ecs::install_hierarchy(world);
        kit::install_kit3d(world, &assets);
        if (writable) extract.init(store, &assets);
        else extract.init(static_cast<const render::MeshStore&>(store), &assets);
        world.spawn_with(LocalTransform{kOrigin, quat::identity(), {1, 1, 1}}, kit::Camera3DComponent{});  // looking down -Z
        root = world.spawn_with(LocalTransform{}, kit::StaticMerge3D{});
    }
    Entity child(asset::Guid g, vec3 p, Entity parent = {}) {
        kit::MeshRef m;
        m.mesh.guid = g;
        const Entity e = world.spawn_with(LocalTransform{kOrigin + p, quat::identity(), {1, 1, 1}}, m);
        world.add(e, ChildOf{parent.is_null() ? root : parent});
        return e;
    }
    // One extract into a built scene; `visit` reads it before it goes.
    template <class F>
    void frame(F&& visit) {
        ecs::propagate_transforms(world);
        render::Camera3D cam;
        REQUIRE(extract.camera(world, 1.0f, cam));
        render::Scene3D scene(store, mem::heap());
        scene.begin(cam, {128, 128});
        extract.extract(world, scene, 1.0f);
        scene.build();
        visit(scene);
    }
    void frame() {
        frame([](const render::Scene3D&) {});
    }
};

// The seven pieces of a small wall at z = -6: three red boxes, two red posts, two green slabs.
void build_wall(Fixture& f) {
    f.child(kRedA, {-1.5f, 0, -6});
    f.child(kRedA, {0.0f, 0, -6});
    f.child(kRedA, {1.5f, 0, -6});
    f.child(kRedB, {-2.2f, 0, -6});
    f.child(kRedB, {2.2f, 0, -6});
    f.child(kGreen, {-0.7f, 0, -4.5f});
    f.child(kGreen, {0.7f, 0, -4.5f});
}

}  // namespace

TEST_CASE("static merge: StaticMerge3D is reflected and installed") {
    const refl::TypeRecord* t = refl::find_type(StringId("StaticMerge3D"));
    REQUIRE(t != nullptr);
    CHECK(t->is_component());
    kit::StaticMerge3D m;
    CHECK(m.cell_size == 32.0f);
    CHECK(m.cast_shadows == 1);
    CHECK(m.enabled == 1);
    CHECK(m.version == 0);
}

TEST_CASE("static merge: qualifying children merge into one draw per material and cell") {
    Fixture f("static_merge_kit_basic");
    build_wall(f);
    const u32 meshes_before = f.store.live_meshes();
    f.frame([&](const render::Scene3D& scene) {
        REQUIRE(scene.instances().size() == 1);  // seven MeshRefs, one merged mesh
        const render::MeshInstance& inst = scene.instances()[0];
        CHECK(inst.mesh >= render::MeshStore::kFirstLocalKey);
        CHECK(inst.cast_shadows);
        CHECK(inst.tint.x == 1.0f);
        const render::GpuMesh* mesh = f.store.get(inst.mesh);
        REQUIRE(mesh != nullptr);
        CHECK(mesh->submeshes.size() == 2);  // red, green
        CHECK(scene.draws().size() == 2);    // materials x cells
        // The merged bounds are the children's, in world space.
        CHECK(mesh->bounds.min.x == doctest::Approx(5.55f).epsilon(0.001));
        CHECK(mesh->bounds.max.x == doctest::Approx(10.45f).epsilon(0.001));
        CHECK(mesh->bounds.max.y == doctest::Approx(2.0f).epsilon(0.001));
        CHECK(mesh->bounds.min.z == doctest::Approx(-6.5f).epsilon(0.001));
        CHECK(mesh->bounds.max.z == doctest::Approx(-4.0f).epsilon(0.001));
    });
    const kit::Kit3DStats& st = f.extract.stats();
    CHECK(st.merged_groups == 1);
    CHECK(st.merged_children == 7);
    CHECK(st.merged_cells == 1);
    CHECK(st.meshes == 0);  // no MeshRef drew on its own
    CHECK(f.store.live_meshes() == meshes_before + 1);

    // Unchanged children: the same merged mesh, no re-merge, no new uploads.
    u32 key = ~0u;
    f.frame([&](const render::Scene3D& scene) { key = scene.instances()[0].mesh; });
    f.frame([&](const render::Scene3D& scene) { CHECK(scene.instances()[0].mesh == key); });
    CHECK(f.store.live_meshes() == meshes_before + 1);
}

TEST_CASE("static merge: an animated, blended, interpolated or hidden child stays out of the merge") {
    Fixture f("static_merge_kit_separate");
    build_wall(f);
    const Entity glass = f.child(kGlass, {0, 0, -3});
    const Entity spinner = f.child(kSpin, {3, 0, -5});
    f.world.add(spinner, kit::Animator{});
    const Entity moving = f.child(kRedA, {-3, 0, -5});
    f.world.try_get_mut<kit::MeshRef>(moving)->interpolate = 1;
    const Entity hidden = f.child(kRedA, {4, 0, -5});
    f.world.try_get_mut<kit::MeshRef>(hidden)->visible = 0;
    const Entity nested = f.child(kGreen, {0, 0, -8}, glass);  // a grandchild merges, whatever its parent does
    (void)nested;
    f.frame([&](const render::Scene3D& scene) {
        // The merged cell, the glass, the animated spinner and the interpolated box: four draws' instances.
        CHECK(scene.instances().size() == 4);
        u32 merged = 0;
        for (const render::MeshInstance& i : scene.instances()) merged += i.mesh >= render::MeshStore::kFirstLocalKey ? 1u : 0u;
        CHECK(merged == 1);
    });
    CHECK(f.extract.stats().merged_children == 8);  // the wall's seven and the grandchild
    CHECK(f.extract.stats().meshes == 3);
}

TEST_CASE("static merge: moving a child, or bumping version, re-merges and retires the old mesh") {
    Fixture f("static_merge_kit_remerge");
    build_wall(f);
    const Entity mover = f.child(kRedA, {0, 0, -9});
    const u32 meshes_before = f.store.live_meshes() + 1;  // one merged mesh joins the store
    u32 first = ~0u;
    f.frame([&](const render::Scene3D& scene) {
        REQUIRE(scene.instances().size() == 1);
        first = scene.instances()[0].mesh;
        CHECK(f.store.get(first)->bounds.min.z == doctest::Approx(-9.5f).epsilon(0.001));
    });
    // Move one child 10 m away (still inside the cell): a new mesh, the old key gone.
    f.world.try_get_mut<LocalTransform>(mover)->position = kOrigin + vec3{0, 0, -16};
    u32 second = ~0u;
    f.frame([&](const render::Scene3D& scene) {
        REQUIRE(scene.instances().size() == 1);
        second = scene.instances()[0].mesh;
        CHECK(second != first);
        CHECK(f.store.get(second)->bounds.min.z == doctest::Approx(-16.5f).epsilon(0.001));
    });
    CHECK(f.store.get(first) == nullptr);
    CHECK(f.store.live_meshes() == meshes_before);
    // version alone re-merges too.
    f.world.try_get_mut<kit::StaticMerge3D>(f.root)->version = 1;
    u32 third = ~0u;
    f.frame([&](const render::Scene3D& scene) { third = scene.instances()[0].mesh; });
    CHECK(third != second);
    CHECK(f.store.get(second) == nullptr);
    CHECK(f.store.live_meshes() == meshes_before);
    // A child leaving the group (loses its MeshRef) re-merges as well, and the rest still merge.
    f.world.remove<kit::MeshRef>(mover);
    f.frame([&](const render::Scene3D& scene) {
        REQUIRE(scene.instances().size() == 1);
        CHECK(f.store.get(scene.instances()[0].mesh)->bounds.min.z == doctest::Approx(-6.5f).epsilon(0.001));
    });
    CHECK(f.extract.stats().merged_children == 7);
}

TEST_CASE("static merge: removing or disabling the component, or turning merging off, restores the draws") {
    Fixture f("static_merge_kit_restore");
    build_wall(f);
    const u32 meshes_before = f.store.live_meshes();
    f.frame([&](const render::Scene3D& scene) { CHECK(scene.instances().size() == 1); });
    CHECK(f.store.live_meshes() == meshes_before + 1);

    f.extract.set_static_merge(false);  // what the editor viewport does
    CHECK(f.store.live_meshes() == meshes_before);
    f.frame([&](const render::Scene3D& scene) { CHECK(scene.instances().size() == 7); });
    CHECK(f.extract.stats().merged_groups == 0);
    f.extract.set_static_merge(true);
    f.frame([&](const render::Scene3D& scene) { CHECK(scene.instances().size() == 1); });

    f.world.try_get_mut<kit::StaticMerge3D>(f.root)->enabled = 0;
    f.frame([&](const render::Scene3D& scene) { CHECK(scene.instances().size() == 7); });
    CHECK(f.store.live_meshes() == meshes_before);
    f.world.try_get_mut<kit::StaticMerge3D>(f.root)->enabled = 1;
    f.frame([&](const render::Scene3D& scene) { CHECK(scene.instances().size() == 1); });

    f.world.remove<kit::StaticMerge3D>(f.root);
    f.frame([&](const render::Scene3D& scene) {
        CHECK(scene.instances().size() == 7);
        for (const render::MeshInstance& i : scene.instances()) CHECK(i.mesh < render::MeshStore::kFirstLocalKey);
    });
    CHECK(f.store.live_meshes() == meshes_before);
    CHECK(f.extract.stats().merged_children == 0);
}

TEST_CASE("static merge: cells split by cell_size, cast_shadows 0 silences them, a far cell is culled") {
    Fixture f("static_merge_kit_cells");
    f.world.try_get_mut<kit::StaticMerge3D>(f.root)->cell_size = 16.0f;
    f.child(kRedA, {-2, 0, -6});
    f.child(kGreen, {2, 0, -6});
    f.child(kRedA, {-2, 0, -26});  // the next cell down -Z, still in view
    f.child(kRedA, {-2, 0, 40});   // behind the camera: its cell is culled
    f.frame([&](const render::Scene3D& scene) {
        CHECK(f.extract.stats().merged_cells == 3);
        CHECK(f.extract.stats().merged_children == 4);
        CHECK(scene.instances().size() == 2);
        for (const render::MeshInstance& i : scene.instances()) CHECK(i.cast_shadows);
    });
    f.world.try_get_mut<kit::StaticMerge3D>(f.root)->cast_shadows = 0;
    f.frame([&](const render::Scene3D& scene) {
        REQUIRE(scene.instances().size() == 2);
        for (const render::MeshInstance& i : scene.instances()) CHECK_FALSE(i.cast_shadows);
    });
    // A child that opts out of shadows is its own cell next to the casters.
    f.world.try_get_mut<kit::StaticMerge3D>(f.root)->cast_shadows = 1;
    const Entity dark = f.child(kRedA, {-3, 0, -6});
    f.world.try_get_mut<kit::MeshRef>(dark)->cast_shadows = 0;
    f.frame([&](const render::Scene3D& scene) {
        u32 casting = 0, silent = 0;
        for (const render::MeshInstance& i : scene.instances()) (i.cast_shadows ? casting : silent)++;
        CHECK(casting == 2);
        CHECK(silent == 1);
    });
}

TEST_CASE("static merge: a tint is folded into the material and a material override takes its own submesh") {
    const std::string src = cook_test::fresh_dir("static_merge_kit_material");
    cook_test::write_file(src + "/assets/blue.mcmat", R"({ "format": "mce-material", "version": 1,
        "guid": "5b000000-0000-4000-8000-000000000071", "base_color": [0, 0, 1, 1], "roughness": 0.3 })");
    cook::CookService svc;
    REQUIRE(svc.cook(StringView(src.c_str()), cook::CookOptions{}).ok());
    const std::string cooked = src + "/cooked";
    Fixture f("static_merge_kit_tint", true, cooked);
    const Entity a = f.child(kRedA, {-1, 0, -6});
    const Entity b = f.child(kRedA, {1, 0, -6});
    f.world.try_get_mut<kit::MeshRef>(b)->tint = {0.5f, 1.0f, 1.0f, 1.0f};
    (void)a;
    const Entity c = f.child(kRedA, {3, 0, -6});
    f.world.try_get_mut<kit::MeshRef>(c)->material.guid = asset::parse_guid("5b000000-0000-4000-8000-000000000071").value();
    f.frame([&](const render::Scene3D& scene) {
        REQUIRE(scene.instances().size() == 1);
        const render::GpuMesh* m = f.store.get(scene.instances()[0].mesh);
        REQUIRE(m != nullptr);
        REQUIRE(m->submeshes.size() == 3);
        CHECK(m->submeshes[0].material.base_color[0] == doctest::Approx(0.8f));
        CHECK(m->submeshes[1].material.base_color[0] == doctest::Approx(0.4f));
        CHECK(m->submeshes[2].material.base_color[0] == doctest::Approx(0.0f));  // the override's blue
        CHECK(m->submeshes[2].material.base_color[2] == doctest::Approx(1.0f));
        CHECK(m->submeshes[2].material.roughness == doctest::Approx(0.3f));
        CHECK(scene.instances()[0].tint.x == 1.0f);
        CHECK(scene.instances()[0].material == nullptr);
    });
}

TEST_CASE("static merge: a read-only store cannot hold merged meshes, so the children keep drawing") {
    Fixture f("static_merge_kit_readonly", false);
    build_wall(f);
    f.frame([&](const render::Scene3D& scene) { CHECK(scene.instances().size() == 7); });
    CHECK(f.extract.stats().merged_groups == 0);
}

TEST_CASE("static merge: shutdown and reset release the merged meshes") {
    Fixture f("static_merge_kit_shutdown");
    build_wall(f);
    const u32 meshes_before = f.store.live_meshes();
    f.frame();
    CHECK(f.store.live_meshes() == meshes_before + 1);
    f.extract.reset(&f.assets);
    CHECK(f.store.live_meshes() == meshes_before);
    f.frame();  // merges again after the reset
    CHECK(f.store.live_meshes() == meshes_before + 1);
    f.extract.shutdown();
    CHECK(f.store.live_meshes() == meshes_before);
}

// The M-OPENWORLD city gate (A4), by counts: a block of 4096 static pieces reaches the scene as its cells, and an
// unchanged block is not merged again (release build, 24k pieces: 15 ms of Scene3D::build one by one, 0.6 ms merged;
// the per-frame check of an unchanged block went from 10 to 3 ms with the merge-time reads moved out of it).
TEST_CASE("static merge: a 4096-piece city block reaches the scene as its cells and merges again only on a change") {
    Fixture f("static_merge_kit_city");
    const u32 side = 64;
    Entity last;
    for (u32 i = 0; i < side * side; ++i) {
        const asset::Guid g = i % 3 == 0 ? kRedA : i % 3 == 1 ? kRedB : kGreen;
        const u32 row = i / side, column = i % side;
        last = f.child(g, {f32(column) * 6.0f - 192.0f, 0.0f, -f32(row) * 6.0f - 10.0f});
    }
    usize instances = 0;
    f.frame([&](const render::Scene3D& scene) { instances = scene.instances().size(); });
    const kit::Kit3DStats first = f.extract.stats();
    CHECK(first.merged_rebuilt == 1);
    CHECK(first.merged_children == side * side);
    CHECK(first.meshes == 0);                     // no piece draws on its own
    CHECK(first.merged_cells == 13u * 13u);  // ~384 m square in 32 m cells, one mesh per cell (a submesh per material)
    CHECK(instances > 0);
    CHECK(instances < first.merged_cells);  // the cells behind the camera are culled as cells
    f.frame();
    CHECK(f.extract.stats().merged_rebuilt == 0);
    CHECK(f.extract.stats().merged_cells == first.merged_cells);
    f.world.try_get_mut<LocalTransform>(last)->position.y = 1.0f;
    f.frame();
    CHECK(f.extract.stats().merged_rebuilt == 1);
    f.frame();
    CHECK(f.extract.stats().merged_rebuilt == 0);
}
