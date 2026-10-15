// The editor's navmesh bake (M-GAMEPLAY3D 1): the scene's colliders (a BoxCollider3D and a
// MeshCollider3D of a cooked model) into assets/navmesh/<scene>/nav_<id>.mcnav + sidecar, the
// NavMeshSurface3D pointed at the cooked bake (one undo step), the GUID kept on re-bake, and the
// orbit view's NavMeshPreview drawing it.

#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "cook_test_util.h"
#include "mce/editor/editor_session.h"
#include "mce/editor/orbit_camera.h"
#include "mce/kit/kit3d.h"
#include "mce/kit/nav3d.h"

using namespace mce;
using namespace mce::editor;
using namespace cook_test;
namespace fs = std::filesystem;

namespace {

constexpr const char* kFloorGuid = "292d58ee-2d67-59bc-a7ea-e3195335febe";  // projects/Room3D models

std::string nav_project() {
    const std::string root = fresh_dir("navmesh_bake");
    const std::string models = std::string(MCE_SOURCE_DIR) + "/projects/Room3D/assets/models/";
    fs::create_directories(root + "/assets/models");
    for (const char* f : {"floor.glb", "floor.glb.mceasset"}) fs::copy_file(models + f, root + "/assets/models/" + f);
    fs::copy(std::string(MCE_SOURCE_DIR) + "/projects/Room3D/assets/textures", root + "/assets/textures");
    return root;
}

}  // namespace

TEST_CASE("editor navmesh bake: .mcnav + sidecar, the surface pointed at it, the orbit preview draws it") {
    EditorSession session;
    const std::string root = nav_project();
    REQUIRE(session.open_project(root).has_value());
    EditorScene& s = session.scene();

    // No surface: the bake says so.
    Result<physics::NavBakeStats3D> none = session.bake_navmesh();
    REQUIRE_FALSE(none);

    const EntityId floor = s.create_entity("Floor");
    LocalTransform ft;
    ft.position = {0, -0.5f, 0};
    s.set(floor, ft);
    s.add_component(floor, refl::type_of<kit::BoxCollider3D>());
    kit::BoxCollider3D box = *s.get<kit::BoxCollider3D>(floor);
    box.half = {10, 0.5f, 10};
    s.set(floor, box);

    const EntityId model = s.create_entity("Model floor");
    LocalTransform mt;
    mt.position = {30, 0, 0};
    s.set(model, mt);
    s.add_component(model, refl::type_of<kit::MeshCollider3D>());
    kit::MeshCollider3D mc = *s.get<kit::MeshCollider3D>(model);
    mc.mesh.guid = asset::parse_guid(kFloorGuid).value();
    s.set(model, mc);

    const EntityId surface = s.create_entity("Navmesh");
    s.add_component(surface, refl::type_of<kit::NavMeshSurface3D>());

    Result<physics::NavBakeStats3D> baked = session.bake_navmesh();
    if (!baked) MESSAGE(baked.error().stage, " ", baked.error().path.c_str());
    REQUIRE(baked);
    CHECK(baked.value().layers > 0);
    CHECK(baked.value().triangles > 12);  // the box and the model's triangles
    const asset::Guid guid = s.get<kit::NavMeshSurface3D>(surface)->baked.guid;
    REQUIRE(guid != asset::Guid{});
    const std::string file = root + "/assets/navmesh/untitled/nav_" + std::to_string(surface) + ".mcnav";
    REQUIRE(fs::exists(file));
    REQUIRE(fs::exists(file + ".mceasset"));
    CHECK(fs::file_size(file) == baked.value().bytes);
    CHECK(session.project().mounted()->find(guid) != asset::kInvalidAssetKey);

    // The preview loads it: floor edges, and some on the model 30 m away.
    NavMeshPreview preview;
    preview.sync(s, session.project().mounted());
    REQUIRE(preview.mesh().loaded());
    Array<Segment3D> segs;
    preview.segments(segs);
    CHECK(segs.size() > 6);
    bool on_model = false;
    for (const Segment3D& g : segs) on_model |= g.a.x > 25.0f;
    CHECK(on_model);
    kit::NavMeshSurface3D hidden = *s.get<kit::NavMeshSurface3D>(surface);
    hidden.show = 0;
    s.set(surface, hidden);
    preview.sync(s, session.project().mounted());
    segs.clear();
    preview.segments(segs);
    CHECK(segs.empty());

    // One undo step for the bake's pointer (the show edit first); a re-bake keeps the GUID.
    CHECK(s.undo());
    CHECK(s.undo());
    CHECK(s.get<kit::NavMeshSurface3D>(surface)->baked.guid == asset::Guid{});
    REQUIRE(session.bake_navmesh());
    CHECK(s.get<kit::NavMeshSurface3D>(surface)->baked.guid == guid);
}
