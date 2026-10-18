// The editor's HLOD bake (M-OPENWORLD A3): a SceneChunk's area scene (two cooked floor models, one tiny prop, one
// hidden) merged into assets/hlod/<scene>/chunk_<id>.mchlod + sidecar, relative to the chunk entity; the chunk's
// ChunkLod3D added and pointed at the cooked proxy (one undo step); a re-bake keeps the file, the GUID and the bytes.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "cook_test_util.h"
#include "mce/asset/formats.h"
#include "mce/editor/editor_session.h"
#include "mce/kit/chunk_lod3d.h"
#include "mce/scene/streaming.h"

using namespace mce;
using namespace mce::editor;
using namespace cook_test;
namespace fs = std::filesystem;

namespace {

constexpr const char* kFloorGuid = "292d58ee-2d67-59bc-a7ea-e3195335febe";  // projects/Room3D models
constexpr const char* kAreaGuid = "5ce4e000-0000-4000-8000-0000000004a1";

std::string area_scene() {
    std::string s = R"({ "format": "mce-scene", "version": 1, "guid": ")";
    s += kAreaGuid;
    s += R"(", "name": "block", "entities": [)";
    const char* rows[] = {
        R"({ "id": 1, "name": "A", "components": { "LocalTransform": { "position": [100, 0, 0] }, "MeshRef": { "mesh": "guid:%G" } } })",
        R"({ "id": 2, "name": "B", "components": { "LocalTransform": { "position": [110, 0, 0] }, "MeshRef": { "mesh": "guid:%G" } } })",
        R"({ "id": 3, "name": "Tiny", "components": { "LocalTransform": { "position": [105, 0, 5], "scale": [0.01, 0.01, 0.01] }, "MeshRef": { "mesh": "guid:%G" } } })",
        R"({ "id": 4, "name": "Hidden", "components": { "LocalTransform": { "position": [105, 0, -5] }, "MeshRef": { "mesh": "guid:%G", "visible": 0 } } })",
    };
    for (usize i = 0; i < 4; ++i) {
        std::string r = rows[i];
        r.replace(r.find("%G"), 2, kFloorGuid);
        s += (i ? ", " : "") + r;
    }
    s += "] }\n";
    return s;
}

std::string hlod_project() {
    const std::string root = fresh_dir("hlod_bake");
    const std::string models = std::string(MCE_SOURCE_DIR) + "/projects/Room3D/assets/models/";
    fs::create_directories(root + "/assets/models");
    for (const char* f : {"floor.glb", "floor.glb.mceasset"}) fs::copy_file(models + f, root + "/assets/models/" + f);
    fs::copy(std::string(MCE_SOURCE_DIR) + "/projects/Room3D/assets/textures", root + "/assets/textures");
    write_file(root + "/assets/scenes/block.mcscene", area_scene());
    return root;
}

std::string read_all(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

}  // namespace

TEST_CASE("editor hlod bake: .mchlod + sidecar relative to the chunk, ChunkLod3D pointed at it, re-bakes identical") {
    EditorSession session;
    const std::string root = hlod_project();
    REQUIRE(session.open_project(root).has_value());
    EditorScene& s = session.scene();

    REQUIRE_FALSE(session.bake_hlod({}));  // no chunk yet: says so

    const EntityId chunk = s.create_entity("Block");
    LocalTransform t;
    t.position = {105, 0, 0};
    s.set(chunk, t);
    s.add_component(chunk, refl::type_of<scene::SceneChunk>());
    scene::SceneChunk sc = *s.get<scene::SceneChunk>(chunk);
    sc.scene.guid = asset::parse_guid(kAreaGuid).value();
    sc.half_extents = {20, 10, 20};
    s.set(chunk, sc);

    Result<Array<EditorSession::HlodBakeRow>> baked = session.bake_hlod({});
    if (!baked) MESSAGE(baked.error().stage, " ", baked.error().path.c_str());
    REQUIRE(baked);
    REQUIRE(baked.value().size() == 1);
    const EditorSession::HlodBakeRow& row = baked.value()[0];
    CHECK(row.chunk == chunk);
    CHECK(row.stats.meshes == 4);
    CHECK(row.stats.merged == 2);
    CHECK(row.stats.small == 1);
    CHECK(row.stats.skipped == 1);
    CHECK(row.stats.triangles > 0);
    // Relative to the chunk at x = 105: the two floors around x = -5 and +5.
    CHECK(row.stats.bounds.min.x < -4.0f);
    CHECK(row.stats.bounds.max.x > 4.0f);
    CHECK(row.stats.bounds.max.x < 40.0f);
    REQUIRE(row.proxy != asset::Guid{});
    const std::string file = root + "/assets/hlod/untitled/chunk_" + std::to_string(chunk) + ".mchlod";
    REQUIRE(fs::exists(file));
    REQUIRE(fs::exists(file + ".mceasset"));
    CHECK(fs::file_size(file) == row.bytes);
    CHECK(read_all(file + ".mceasset").find("\"lod_levels\": 2") != std::string::npos);  // far levels where sources had none
    CHECK(session.project().mounted()->find(row.proxy) != asset::kInvalidAssetKey);
    const kit::ChunkLod3D* lod = s.get<kit::ChunkLod3D>(chunk);
    REQUIRE(lod);
    CHECK(lod->proxy.guid == row.proxy);

    // The cooked proxy is an ordinary mesh.
    asset::AssetSystem& assets = *session.project().mounted();
    const Result<asset::AssetHandle<asset::Mesh>> h = assets.load_sync<asset::Mesh>(row.proxy);
    REQUIRE(h);
    CHECK(asset::MeshView::read(assets.resolve(h.value())).has_value());
    assets.release(h.value());

    // One undo step takes the component away again; a re-bake brings it back with the same GUID and bytes.
    const std::string first = read_all(file);
    s.undo();
    CHECK(s.get<kit::ChunkLod3D>(chunk) == nullptr);
    Result<Array<EditorSession::HlodBakeRow>> again = session.bake_hlod({});
    REQUIRE(again);
    CHECK(again.value()[0].proxy == row.proxy);
    CHECK(read_all(file) == first);
    CHECK(s.get<kit::ChunkLod3D>(chunk)->proxy.guid == row.proxy);

    // A min_size above every mesh merges nothing and leaves the chunk as it is.
    kit::HlodBakeSettings3D big;
    big.min_size = 1000.0f;
    Result<Array<EditorSession::HlodBakeRow>> none = session.bake_hlod(big);
    REQUIRE(none);
    CHECK(none.value()[0].proxy == asset::Guid{});
    CHECK(none.value()[0].stats.small == 3);
    CHECK(s.get<kit::ChunkLod3D>(chunk)->proxy.guid == row.proxy);
}

TEST_CASE("editor hlod bake: impostor atlas + mesh next to the proxy, ChunkLod3D impostor pointed at it, re-bakes identical") {
    EditorSession session;
    const std::string root = hlod_project();
    REQUIRE(session.open_project(root).has_value());
    EditorScene& s = session.scene();
    const EntityId chunk = s.create_entity("Block");
    LocalTransform t;
    t.position = {105, 0, 0};
    s.set(chunk, t);
    s.add_component(chunk, refl::type_of<scene::SceneChunk>());
    scene::SceneChunk sc = *s.get<scene::SceneChunk>(chunk);
    sc.scene.guid = asset::parse_guid(kAreaGuid).value();
    sc.half_extents = {20, 10, 20};
    s.set(chunk, sc);

    const cook::ImpostorSettings imp;
    Result<Array<EditorSession::HlodBakeRow>> baked = session.bake_hlod({}, kNoEntity, &imp);
    if (!baked) MESSAGE(baked.error().stage, " ", baked.error().path.c_str());
    REQUIRE(baked);
    const EditorSession::HlodBakeRow& row = baked.value()[0];
    REQUIRE(row.proxy != asset::Guid{});
    REQUIRE(row.impostor != asset::Guid{});
    CHECK(row.impostor_stats.groups == 2);  // the two floors (Tiny is below min_size, Hidden is hidden)
    CHECK(row.impostor_stats.boxes == 2);
    CHECK(row.impostor_stats.quads >= 2);
    CHECK(row.impostor_stats.untextured == 0);
    const std::string base = root + "/assets/hlod/untitled/chunk_" + std::to_string(chunk);
    REQUIRE(fs::exists(base + "_far.png"));
    REQUIRE(fs::exists(base + "_far.png.mceasset"));
    CHECK(read_all(base + "_far.png.mceasset").find("\"importer\": \"texture\"") != std::string::npos);
    REQUIRE(fs::exists(base + "_far.mchlod"));
    CHECK(fs::file_size(base + "_far.mchlod") == row.impostor_bytes);
    const kit::ChunkLod3D* lod = s.get<kit::ChunkLod3D>(chunk);
    REQUIRE(lod);
    CHECK(lod->proxy.guid == row.proxy);
    CHECK(lod->impostor.guid == row.impostor);

    // The impostor is an ordinary mesh whose material names the cooked atlas.
    asset::AssetSystem& assets = *session.project().mounted();
    const Result<asset::AssetHandle<asset::Mesh>> h = assets.load_sync<asset::Mesh>(row.impostor);
    REQUIRE(h);
    const Result<asset::MeshView> v = asset::MeshView::read(assets.resolve(h.value()));
    REQUIRE(v);
    const asset::Guid atlas = v.value().submeshes[0].material.texture(asset::MaterialSlot::BaseColor);
    assets.release(h.value());
    const Result<asset::AssetHandle<asset::Texture>> th = assets.load_sync<asset::Texture>(atlas);
    REQUIRE(th);
    CHECK(asset::TextureView::read(assets.resolve(th.value())).has_value());
    assets.release(th.value());

    // A re-bake keeps both GUIDs and both files' bytes.
    const std::string png = read_all(base + "_far.png"), mesh = read_all(base + "_far.mchlod");
    Result<Array<EditorSession::HlodBakeRow>> again = session.bake_hlod({}, kNoEntity, &imp);
    REQUIRE(again);
    CHECK(again.value()[0].impostor == row.impostor);
    CHECK(read_all(base + "_far.png") == png);
    CHECK(read_all(base + "_far.mchlod") == mesh);
}
