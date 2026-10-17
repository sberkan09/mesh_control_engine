// asset_import (M-IMPORT step 4) in process against a copy of the scene_demo project: the Tiled and
// Aseprite converters run as the real mce_import.py, the written assets are registered at once and
// cook, a re-import keeps the GUIDs, and bad arguments and --werror come back as errors.

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "mcp_client.h"

using namespace mce;
using namespace mcp_client;
using namespace cook_test;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void use_source_tree_importer() {
    const std::string script = std::string(MCE_SOURCE_DIR) + "/tools/mce_cli/mce_import.py";
#if defined(_WIN32)
    _putenv_s("MCE_IMPORT_SCRIPT", script.c_str());
#else
    setenv("MCE_IMPORT_SCRIPT", script.c_str(), 1);
#endif
}

std::string import_project(const char* name) {
    const std::string root = fresh_dir(name);
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    write_file(root + "/assets/levels/terrain.png", make_png(4, 2, std::vector<u8>(4 * 2 * 4, 255)));
    write_file(root + "/assets/levels/cave.tmj", R"({
  "type": "map", "orientation": "orthogonal", "infinite": false, "width": 2, "height": 2, "tilewidth": 1, "tileheight": 1,
  "tilesets": [{"firstgid": 1, "name": "terrain", "image": "terrain.png", "imagewidth": 4, "imageheight": 2,
                "tilewidth": 1, "tileheight": 1, "tilecount": 8, "columns": 4, "margin": 0, "spacing": 0}],
  "layers": [{"type": "tilelayer", "name": "Ground", "width": 2, "height": 2, "visible": true, "opacity": 1, "data": [0, 0, 1, 2]},
             {"type": "objectgroup", "name": "Things", "objects": [{"id": 1, "name": "Start", "x": 1, "y": 1, "point": true}]}]
})");
    return root;
}

}  // namespace

TEST_CASE("asset_import: a Tiled map becomes a scene and a tilemap, re-imports keep GUIDs, bad calls are errors") {
    use_source_tree_importer();
    const std::string root = import_project("mcp_asset_import");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();

    auto done = c.ok("asset_import", R"({"tool":"tiled","file":"assets/levels/cave.tmj"})");
    CHECK(done.v().get("ok").as_bool());
    CHECK(str(done.v().get("out_dir")) == "assets/levels");
    const std::string map = root + "/assets/levels/cave_Ground.mctilemap";
    REQUIRE(std::filesystem::exists(map));
    REQUIRE(std::filesystem::exists(root + "/assets/levels/cave.mcscene"));
    CHECK(slurp(map).find("\"mce-tilemap\"") != std::string::npos);

    // Registered at once: the asset tools see the new files.
    auto read = c.ok("asset_read", R"({"path":"levels/cave_Ground.mctilemap"})");
    CHECK(str(read.v().get("type")) == "Tilemap");

    // A second import gives the same bytes (GUIDs kept).
    const std::string first = slurp(map) + slurp(root + "/assets/levels/cave.mcscene");
    c.ok("asset_import", R"({"tool":"tiled","file":"assets/levels/cave.tmj"})");
    CHECK(first == slurp(map) + slurp(root + "/assets/levels/cave.mcscene"));

    // The export may sit outside assets/; the assets go where out_dir says.
    write_file(root + "/art/cave2.tmj", slurp(root + "/assets/levels/cave.tmj"));
    write_file(root + "/art/terrain.png", slurp(root + "/assets/levels/terrain.png"));
    CHECK(c.fails("asset_import", R"({"tool":"tiled","file":"art/cave2.tmj","out_dir":"elsewhere"})").find("under assets/") != std::string::npos);
    c.ok("asset_import", R"({"tool":"tiled","file":"art/cave2.tmj","out_dir":"assets/imp","tile_size":2})");
    CHECK(std::filesystem::exists(root + "/assets/imp/cave2.mcscene"));

    // --werror: a warning fails and writes nothing.
    std::string flip = slurp(root + "/art/cave2.tmj");
    flip.replace(flip.find("[0, 0, 1, 2]"), 12, "[0, 0, 1, 2147483650]");
    write_file(root + "/art/flip.tmj", flip);
    write_file(root + "/art/terrain.png", slurp(root + "/assets/levels/terrain.png"));
    CHECK(c.fails("asset_import", R"({"tool":"tiled","file":"art/flip.tmj","out_dir":"assets/flip","werror":true})").find("flipped") !=
          std::string::npos);
    CHECK(!std::filesystem::exists(root + "/assets/flip"));
    auto warned = c.ok("asset_import", R"({"tool":"tiled","file":"art/flip.tmj","out_dir":"assets/flip"})");
    CHECK(warned.v().get("warnings").as_number() >= 1);

    // Aseprite: the sheet goes to out_dir (a folder that does not exist yet) under the export's name.
    write_file(root + "/art/hero.png", make_png(32, 16, std::vector<u8>(32 * 16 * 4, 255)));
    write_file(root + "/art/hero.json", R"({"frames": [
  {"filename": "a", "frame": {"x": 0, "y": 0, "w": 16, "h": 16}, "rotated": false, "trimmed": false,
   "spriteSourceSize": {"x": 0, "y": 0, "w": 16, "h": 16}, "sourceSize": {"w": 16, "h": 16}, "duration": 100},
  {"filename": "b", "frame": {"x": 16, "y": 0, "w": 16, "h": 16}, "rotated": false, "trimmed": false,
   "spriteSourceSize": {"x": 0, "y": 0, "w": 16, "h": 16}, "sourceSize": {"w": 16, "h": 16}, "duration": 100}],
  "meta": {"image": "hero.png", "size": {"w": 32, "h": 16}, "frameTags": [{"name": "walk", "from": 0, "to": 1, "direction": "forward"}]}})");
    c.ok("asset_import", R"({"tool":"aseprite","file":"art/hero.json","out_dir":"assets/sprites"})");
    REQUIRE(std::filesystem::exists(root + "/assets/sprites/hero.mcsheet"));
    CHECK(str(c.ok("asset_read", R"({"path":"sprites/hero.mcsheet"})").v().get("type")) == "SpriteSheet");

    CHECK(c.fails("asset_import", R"({"tool":"nope","file":"art/cave2.tmj"})").find("'tool'") != std::string::npos);
    CHECK(c.fails("asset_import", R"({"tool":"tiled","file":"../etc/passwd"})").find("'file'") != std::string::npos);
    CHECK(c.fails("asset_import", R"({"tool":"tiled","file":"art/missing.tmj"})").find("no file") != std::string::npos);
    CHECK(c.fails("asset_import", R"({"tool":"tiled","file":"art/cave2.tmj","name":"a b"})").find("'name'") != std::string::npos);
    CHECK(c.fails("asset_import", R"({"tool":"tiled","file":"art/cave2.tmj","level":"x"})").find("ldtk") != std::string::npos);
    CHECK(c.fails("asset_import", R"({"tool":"aseprite","file":"art/cave2.tmj","tile_size":2})").find("'tile_size'") != std::string::npos);
}

// M-IMPORT3D step 4: tool "gltf" / "fbx" write a model's lights and cameras as a prefab beside the model.
TEST_CASE("asset_import: a glTF model's lights and camera become a prefab, re-imports keep GUIDs, fbx and bad calls are errors") {
    use_source_tree_importer();
    const std::string root = import_project("mcp_asset_import_3d");
    const std::string model = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
  "nodes":[{"name":"room","children":[1,2,3]},
           {"name":"Bulb","translation":[1,2,3],"extensions":{"KHR_lights_punctual":{"light":0}}},
           {"name":"Sun","extensions":{"KHR_lights_punctual":{"light":1}}},
           {"name":"Eye","camera":0,"translation":[0,1,5]}],
  "cameras":[{"type":"perspective","perspective":{"yfov":0.8,"znear":0.1,"zfar":100}}],
  "extensionsUsed":["KHR_lights_punctual"],
  "extensions":{"KHR_lights_punctual":{"lights":[{"type":"point","intensity":2145.6},{"type":"directional"}]}}})";
    write_file(root + "/assets/models/room.gltf", model);
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();

    auto done = c.ok("asset_import", R"({"tool":"gltf","file":"assets/models/room.gltf"})");
    CHECK(done.v().get("ok").as_bool());
    CHECK(str(done.v().get("out_dir")) == "assets/models");
    CHECK(done.v().get("warnings").as_number() == 1);  // the directional light
    CHECK(str(done.v().get("output")).find("Environment3D.sun_direction") != std::string::npos);
    const std::string prefab = root + "/assets/models/room.mcprefab";
    REQUIRE(std::filesystem::exists(prefab));
    REQUIRE(std::filesystem::exists(root + "/assets/models/room.gltf.mceasset"));
    const std::string text = slurp(prefab);
    CHECK(text.find("\"mce-prefab\"") != std::string::npos);
    CHECK(text.find("PointLight3D") != std::string::npos);
    CHECK(text.find("Camera3DComponent") != std::string::npos);
    CHECK(text.find("Sun") == std::string::npos);

    // Registered at once, and a second import gives the same bytes (GUIDs kept).
    CHECK(str(c.ok("asset_read", R"({"path":"models/room.mcprefab"})").v().get("type")) == "Prefab");
    const std::string sidecar = slurp(root + "/assets/models/room.gltf.mceasset");
    c.ok("asset_import", R"({"tool":"gltf","file":"assets/models/room.gltf"})");
    CHECK(text == slurp(prefab));
    CHECK(sidecar == slurp(root + "/assets/models/room.gltf.mceasset"));

    // --werror fails on the sun and writes nothing new.
    CHECK(c.fails("asset_import", R"({"tool":"gltf","file":"assets/models/room.gltf","name":"strict","werror":true})").find("Environment3D.sun_direction") !=
          std::string::npos);
    CHECK(!std::filesystem::exists(root + "/assets/models/strict.mcprefab"));

    // A corrupt model and an FBX that is not one are failures, not crashes.
    write_file(root + "/assets/models/bad.glb", "glTF\x02\x00\x00\x00");
    CHECK(c.fails("asset_import", R"({"tool":"gltf","file":"assets/models/bad.glb"})").find("truncated") != std::string::npos);
    write_file(root + "/assets/models/bad.fbx", "this is not an FBX file");
    CHECK(!c.fails("asset_import", R"({"tool":"fbx","file":"assets/models/bad.fbx"})").empty());
    CHECK(!std::filesystem::exists(root + "/assets/models/bad.mcprefab"));

    // Argument rules.
    CHECK(c.fails("asset_import", R"({"tool":"gltf","file":"art/room.gltf"})").find("inside assets/") != std::string::npos);
    CHECK(c.fails("asset_import", R"({"tool":"fbx","file":"assets/models/room.gltf","tile_size":2})").find("'tile_size'") != std::string::npos);
    CHECK(c.fails("asset_import", R"({"tool":"gltf","file":"assets/models/room.gltf","level":"x"})").find("ldtk") != std::string::npos);
}
