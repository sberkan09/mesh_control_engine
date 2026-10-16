// The content tools (ROADMAP M-AGENT step 5) in process against a copy of the scene_demo project:
// the generators' previews and saves (sidecar, CREDITS.md row, a cook with no adopt step) and the
// content library served from the source tree's library/library.json.

#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "mcp_client.h"

using namespace mce;
using namespace mcp_client;
using namespace cook_test;

namespace {

const std::string kDemoAssets = std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets";

std::string demo_project(const char* name) {
    const std::string root = fresh_dir(name);
    std::filesystem::copy(kDemoAssets, root + "/assets", std::filesystem::copy_options::recursive);
    return root;
}

std::string colors_of(const Client::Tool& t) {
    std::string s;
    const cook::JsonValue colors = t.v().get("colors");
    for (u32 i = 0; i < colors.size(); ++i) s += str(colors.at(i)) + " ";
    return s;
}

// Joins pieces into one string (tool arguments built from variables).
std::string cat(std::initializer_list<std::string_view> parts) {
    std::string out;
    for (std::string_view p : parts) out += p;
    return out;
}

bool listed(Client& c, const std::string& path) {
    auto list = c.ok("asset_list");
    const cook::JsonValue assets = list.v().get("assets");
    for (u32 i = 0; i < assets.size(); ++i)
        if (str(assets.at(i).get("path")) == path) return true;
    return false;
}

}  // namespace

TEST_CASE("gen_palette, gen_sprite, gen_tileset, gen_texture: seeded, previewed, arguments checked") {
    Client c;
    c.init();
    // The schemas are valid JSON objects (a raw-string slip breaks the whole tools/list).
    auto list = c.request("tools/list");
    const cook::JsonValue tools = list->root().get("result").get("tools");
    REQUIRE(tools.is_array());
    for (u32 i = 0; i < tools.size(); ++i) CHECK_MESSAGE(tools.at(i).get("inputSchema").is_object(), str(tools.at(i).get("name")));
    auto a = c.ok("gen_palette", R"({"seed":4,"count":5,"scheme":"earth"})");
    CHECK(a.v().get("colors").size() == 5);
    CHECK(colors_of(a) == colors_of(c.ok("gen_palette", R"({"seed":4,"count":5,"scheme":"earth"})")));
    CHECK(colors_of(a) != colors_of(c.ok("gen_palette", R"({"seed":5,"count":5,"scheme":"earth"})")));
    REQUIRE(a.images.size() == 1);
    CHECK(a.images[0].width == 5 * 48);

    auto s = c.ok("gen_sprite", R"({"seed":9,"kind":"character","width":12,"height":16,"variants":3,"colors":["#202030","#806040","#e0c080"]})");
    CHECK(s.v().get("frames").size() == 3);
    CHECK(s.v().get("frames").at(2).get("x").as_number() == 24);
    REQUIRE(s.images.size() == 1);
    CHECK(s.images[0].width % 36 == 0);  // the sheet, enlarged a whole number of times
    auto t = c.ok("gen_tileset", R"({"style":"brick","tile":8})");
    CHECK(t.v().get("tiles").size() == 8);
    CHECK(str(t.v().get("tiles").at(3).get("name")) == "top");
    auto n = c.ok("gen_texture", R"({"kind":"worley","width":64,"height":32,"colors":["#000000","#ffffff"]})");
    REQUIRE(n.images.size() == 1);
    CHECK(n.images[0].width == 64);
    CHECK(n.images[0].height == 32);
    auto sky = c.ok("gen_texture", R"({"kind":"sky","stars":0.5})");
    CHECK(sky.v().get("width").as_number() == 1024);
    CHECK(sky.images[0].width == 512);  // the preview is scaled down

    CHECK(c.fails("gen_sprite", R"({"kind":"dragon"})").find("'kind'") != std::string::npos);
    CHECK(c.fails("gen_sprite", R"({"colors":["#000000","#ffffff"]})").find("at least 3") != std::string::npos);
    CHECK(c.fails("gen_palette", R"({"colors":5,"count":99})").find("'count'") != std::string::npos);
    CHECK(c.fails("gen_texture", R"({"kind":"sky","width":512,"height":512})").find("twice") != std::string::npos);
    CHECK(c.fails("gen_tileset", R"({"colors":["#000000"]})").find("4 fill") != std::string::npos);
    // Saving needs a project.
    CHECK(c.fails("gen_palette", R"({"save":"p.png"})").find("--project") != std::string::npos);
}

TEST_CASE("gen_sfx and gen_music: sfxr parameters back, exact loop length, a waveform") {
    Client c;
    c.init();
    auto s = c.ok("gen_sfx", R"({"seed":3,"preset":"laser"})");
    CHECK(s.v().get("seconds").as_number() > 0.05);
    CHECK(s.v().get("seconds").as_number() < 3.0);
    CHECK(s.v().get("peak").as_number() > 0.05);
    CHECK(s.v().get("params").get("base_freq").is_number());
    REQUIRE(s.images.size() == 1);
    auto sine = c.ok("gen_sfx", R"({"seed":3,"preset":"laser","params":{"wave":"sine","env_decay":0.1}})");
    CHECK(str(sine.v().get("params").get("wave")) == "sine");
    CHECK(sine.v().get("params").get("env_decay").as_number() == doctest::Approx(0.1));
    CHECK(c.fails("gen_sfx", R"({"params":{"bass":1}})").find("unknown sfxr parameter 'bass'") != std::string::npos);
    CHECK(c.fails("gen_sfx", R"({"params":{"duty":3}})").find("params.duty") != std::string::npos);

    auto m = c.ok("gen_music", R"({"seed":2,"bpm":120,"bars":1,"scale":"minor"})");
    CHECK(m.v().get("seconds").as_number() == doctest::Approx(2.0));
    CHECK(m.v().get("peak").as_number() > 0.1);
    CHECK(m.v().get("peak").as_number() <= 1.0);
    CHECK(c.fails("gen_music", R"({"bpm":60,"bars":32})").find("60 seconds") != std::string::npos);
}

TEST_CASE("saving generated content: file, sidecar, CREDITS.md row, cooks with no adopt, GUID kept on overwrite") {
    const std::string root = demo_project("mcp_content_save");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    // A drawn sprite: rows of characters through a palette.
    auto drawn = c.ok("gen_pixels", R"({"rows":[".ab.","abba",".ab."],"palette":{"a":"#102030","b":"#ffcc00ff"},"save":"sprites/lamp.png"})");
    REQUIRE(drawn.images.size() == 1);
    CHECK(drawn.v().get("width").as_number() == 4);
    const auto lamp = cook::decode_png(Span<const byte>(reinterpret_cast<const byte*>(read_file(root + "/assets/sprites/lamp.png").data()),
                                                        read_file(root + "/assets/sprites/lamp.png").size()));
    REQUIRE(lamp);
    CHECK(lamp.value().height == 3);
    CHECK(lamp.value().rgba[3] == 0);                                  // '.' is transparent
    CHECK(lamp.value().rgba[4] == 0x10);                               // 'a'
    CHECK(lamp.value().rgba[(1 * 4 + 1) * 4] == 0xff);                 // 'b'
    CHECK(c.fails("gen_pixels", R"({"rows":["ab","a"],"palette":{"a":"#000000","b":"#ffffff"}})").find("row 1") != std::string::npos);
    CHECK(c.fails("gen_pixels", R"({"rows":["az"],"palette":{"a":"#000000"}})").find("'z'") != std::string::npos);
    CHECK(c.fails("gen_pixels", R"({"rows":["a"],"palette":{"ab":"#000000"}})").find("'palette'") != std::string::npos);
    // Pretty-printed tool JSON still goes out as one line (asset_schema's templates).
    c.ok("asset_schema", R"({"type":"Tilemap"})");

    auto s = c.ok("gen_sprite", R"({"seed":2,"kind":"blob","save":"sprites/slime.png"})");
    const std::string guid = str(s.v().get("saved").get("guid"));
    CHECK(guid.find("guid:") == 0);
    CHECK(std::filesystem::exists(root + "/assets/sprites/slime.png"));
    const std::string side = read_file(root + "/assets/sprites/slime.png.mceasset");
    CHECK(side.find("\"importer\": \"texture\"") != std::string::npos);
    CHECK(side.find("\"filter\": \"nearest\"") != std::string::npos);
    CHECK(side.find(guid.substr(5)) != std::string::npos);
    c.ok("gen_sfx", R"({"preset":"jump","save":"sfx/jump2.wav"})");
    c.ok("gen_texture", R"({"kind":"fbm","width":32,"height":32,"save":"textures/rock.png"})");
    const std::string credits = read_file(root + "/CREDITS.md");
    CHECK(credits.find("| `assets/sprites/slime.png` | generated by gen_sprite (seed 2, blob 16x16) |") != std::string::npos);
    CHECK(credits.find("`assets/sfx/jump2.wav`") != std::string::npos);

    // Already there: refused without overwrite; with it the GUID stays and the row is replaced.
    CHECK(c.fails("gen_sprite", R"({"seed":3,"kind":"blob","save":"sprites/slime.png"})").find("overwrite") != std::string::npos);
    auto again = c.ok("gen_sprite", R"({"seed":3,"kind":"blob","save":"sprites/slime.png","overwrite":true})");
    CHECK(str(again.v().get("saved").get("guid")) == guid);
    CHECK(again.v().get("saved").get("replaced").as_bool());
    const std::string credits2 = read_file(root + "/CREDITS.md");
    CHECK(credits2.find("seed 3, blob") != std::string::npos);
    CHECK(credits2.find("seed 2, blob") == std::string::npos);

    auto cook = c.ok("cook_run");
    CHECK(cook.v().get("ok").as_bool());
    CHECK(listed(c, "sprites/slime.png"));
    CHECK(listed(c, "sfx/jump2.wav"));
    CHECK(c.fails("gen_palette", R"({"save":"../out.png"})").find("under assets/") != std::string::npos);
    CHECK(c.fails("gen_sfx", R"({"save":"sfx/x.png"})").find(".wav") != std::string::npos);
}

TEST_CASE("library_search and library_add: CC0 models with their texture, OFL fonts with the license, recipes") {
    const std::string root = demo_project("mcp_content_library");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    auto roads = c.ok("library_search", R"({"query":"road corner"})");
    REQUIRE(roads.v().get("entries").size() >= 1);
    CHECK(str(roads.v().get("entries").at(0).get("id")) == "kenney-road-corner");
    CHECK(str(roads.v().get("entries").at(0).get("license")) == "CC0 1.0");
    CHECK(c.ok("library_search", R"({"kind":"font","license":"OFL"})").v().get("total").as_number() == 8);
    CHECK(c.ok("library_search", R"({"query":"sky","kind":"recipe"})").v().get("total").as_number() == 3);
    CHECK(c.ok("library_search", R"({"query":"zzzz"})").v().get("total").as_number() == 0);

    auto coin = c.ok("library_add", R"({"id":"kenney-coin"})");
    CHECK(coin.v().get("files").size() == 2);
    CHECK(std::filesystem::exists(root + "/assets/models/coin.glb.mceasset"));
    CHECK(std::filesystem::exists(root + "/assets/models/Textures/colormap.png"));
    auto flag = c.ok("library_add", R"({"id":"kenney-flag","path":"props/"})");
    CHECK(std::filesystem::exists(root + "/assets/props/flag.glb"));
    auto flag2 = c.ok("library_add", R"({"id":"kenney-cloud"})");
    CHECK(flag2.v().get("files").at(1).get("kept").as_bool());  // the colormap is already there
    c.ok("library_add", R"({"id":"font-crimson-pro","path":"fonts/body.ttf"})");
    CHECK(std::filesystem::exists(root + "/assets/fonts/body.ttf"));
    CHECK(std::filesystem::exists(root + "/assets/fonts/CrimsonPro-OFL.txt"));
    auto sky = c.ok("library_add", R"({"id":"sky-dusk"})");
    CHECK(str(sky.v().get("saved").get("path")) == "textures/sky-dusk.png");
    REQUIRE(sky.images.size() == 1);

    const std::string credits = read_file(root + "/CREDITS.md");
    CHECK(credits.find("| `assets/models/coin.glb` | Coin by Kenney (kenney.nl, Starter Kit: 3D Platformer") != std::string::npos);
    CHECK(credits.find("| CC0 1.0 |") != std::string::npos);
    CHECK(credits.find("Crimson Pro Regular (serif) by Jacques Le Bailly") != std::string::npos);
    CHECK(credits.find("| SIL OFL 1.1 (`CrimsonPro-OFL.txt`) |") != std::string::npos);
    CHECK(c.ok("cook_run").v().get("ok").as_bool());
    CHECK(listed(c, "models/coin.glb"));

    // asset_delete takes the row with the file, and the license text with the family's last font (SBA-61).
    c.ok("library_add", R"({"id":"font-crimson-pro","path":"fonts/title.ttf"})");
    auto body = c.ok("asset_delete", R"({"path":"fonts/body.ttf"})");
    CHECK(body.v().get("credit_removed").as_bool());
    CHECK_FALSE(body.v().get("license_removed").valid());  // title.ttf's row still names it
    CHECK(std::filesystem::exists(root + "/assets/fonts/CrimsonPro-OFL.txt"));
    auto title = c.ok("asset_delete", R"({"path":"fonts/title.ttf"})");
    CHECK(str(title.v().get("license_removed")) == "fonts/CrimsonPro-OFL.txt");
    CHECK_FALSE(std::filesystem::exists(root + "/assets/fonts/CrimsonPro-OFL.txt"));
    CHECK(c.ok("asset_delete", R"({"path":"textures/sky-dusk.png"})").v().get("credit_removed").as_bool());
    const std::string after = read_file(root + "/CREDITS.md");
    CHECK(after.find("fonts/body.ttf") == std::string::npos);
    CHECK(after.find("fonts/title.ttf") == std::string::npos);
    CHECK(after.find("sky-dusk") == std::string::npos);
    CHECK(after.find("| `assets/models/coin.glb` |") != std::string::npos);  // the other rows stay
    // A row naming two weights loses one name; the folder moves to the name left.
    write_file(root + "/assets/fonts/Serif-Regular.ttf", "x");
    write_file(root + "/assets/fonts/Serif-Bold.ttf", "x");
    write_file(root + "/assets/fonts/Serif-OFL.txt", "x");
    write_file(root + "/CREDITS.md", after + "| `assets/fonts/Serif-Regular.ttf`, `Serif-Bold.ttf` | Serif | SIL OFL 1.1 (`Serif-OFL.txt`) |\n");
    CHECK_FALSE(c.ok("asset_delete", R"({"path":"fonts/Serif-Regular.ttf"})").v().get("license_removed").valid());
    CHECK(read_file(root + "/CREDITS.md").find("| `assets/fonts/Serif-Bold.ttf` | Serif | SIL OFL 1.1 (`Serif-OFL.txt`) |") != std::string::npos);
    CHECK(str(c.ok("asset_delete", R"({"path":"fonts/Serif-Bold.ttf"})").v().get("license_removed")) == "fonts/Serif-OFL.txt");
    CHECK(read_file(root + "/CREDITS.md").find("Serif") == std::string::npos);

    CHECK(c.fails("library_add", R"({"id":"kenney-coin"})").find("already exists") != std::string::npos);
    CHECK(c.fails("library_add", R"({"id":"nope"})").find("library_search") != std::string::npos);
    CHECK(c.fails("library_add", R"({"id":"kenney-coin","path":"x.png"})").find("extension") != std::string::npos);
}

TEST_CASE("gen_mesh: a skinned humanoid walks, carries a prop on a bone and goes limp as a ragdoll") {
    const std::string root = demo_project("mcp_content_humanoid");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    auto t = c.ok("gen_mesh", R"({"shape":"humanoid","color":"#c05030","save":"models/grunt.glb"})");
    REQUIRE(t.images.size() == 1);
    CHECK(t.v().get("bones").size() == 13);
    CHECK(str(t.v().get("clips").at(1)) == "Walk");
    CHECK(t.v().get("size").at(1).as_number() == doctest::Approx(1.8));
    const std::string mesh = str(t.v().get("saved").get("guid"));
    const std::string glb = read_file(root + "/assets/models/grunt.glb");
    u32 json_len = 0;
    std::memcpy(&json_len, glb.data() + 12, 4);
    auto doc = parse(glb.substr(20, json_len));
    CHECK(doc->root().get("skins").at(0).get("joints").size() == 13);
    CHECK(doc->root().get("animations").size() == 2);
    REQUIRE(c.ok("cook_run").v().get("ok").as_bool());

    c.ok("scene_new", R"({"name":"figure"})");
    c.ok("entity_create", R"({"name":"Floor","components":{"LocalTransform":{"position":[0,-0.5,0]},"BoxCollider3D":{"half":[20,0.5,20]}}})");
    const int grunt = static_cast<int>(c.ok("entity_create", cat({R"({"name":"Grunt","components":{"LocalTransform":{"position":[0,0.9,0]},"MeshRef":{"mesh":")",
                                                                     mesh, R"("},"Animator":{"clip":"Walk"},"Ragdoll3D":{}}})"}))
                                           .v().get("id").as_number());
    c.ok("entity_create", cat({R"({"name":"Hand","components":{"LocalTransform":{},"AttachToBone":{"target":"$)", std::to_string(grunt),
                               R"(","bone":"forearm_l"}}})"}));
    c.ok("scene_save", R"({"path":"scenes/figure.mcscene"})");
    const char* args = R"({"ticks":60,"log_lines":0,"inspect":{"entities":["Grunt","Hand"],"show":["Animator","Ragdoll3D","AttachToBone","LocalTransform"]}})";
    auto run = c.ok("run_headless", args);
    cook::JsonValue ents = run.v().get("inspect").get("entities");
    CHECK(ents.at(0).get("components").get("Animator").get("time").as_number() > 0.0);
    CHECK(ents.at(1).get("components").get("AttachToBone").get("found").as_number() == 1);
    // The forearm hangs from the left shoulder: left of and below the shoulder (0.9 m above the origin + 0.54 m).
    const cook::JsonValue hand = ents.at(1).get("components").get("LocalTransform").get("position");
    CHECK(hand.at(0).as_number() == doctest::Approx(0.25).epsilon(0.05));
    CHECK(hand.at(1).as_number() < 0.9 + 0.3 * 1.8);
    CHECK(run.v().get("log").get("errors").as_number() == 0);

    c.ok("component_set", cat({R"({"entity":)", std::to_string(grunt), R"(,"component":"Ragdoll3D","path":"active","value":1})"}));
    c.ok("scene_save");
    run = c.ok("run_headless", R"({"ticks":240,"log_lines":0,"inspect":{"entities":["Grunt"],"show":["Ragdoll3D"]}})");
    const cook::JsonValue rag = run.v().get("inspect").get("entities").at(0).get("components").get("Ragdoll3D");
    CHECK(rag.get("parts").as_number() >= 11);
    CHECK(rag.get("root_position").at(1).as_number() < 0.5);  // fallen over
    CHECK(run.v().get("log").get("errors").as_number() == 0);
}

TEST_CASE("gen_mesh: primitives as .glb that cook, faces wound outwards, a shaded preview") {
    const std::string root = demo_project("mcp_content_mesh");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    for (const char* shape : {"box", "plane", "sphere", "cylinder", "cone", "disc", "ramp"}) {
        INFO(shape);
        const std::string args = std::string(R"({"shape":")") + shape + R"(","size":[2,1,3],"segments":12,"color":"#a0c040","save":"models/)" + shape + R"(.glb"})";
        auto t = c.ok("gen_mesh", args.c_str());
        REQUIRE(t.images.size() == 1);
        CHECK(t.v().get("triangles").as_number() >= 1);
        // Read the .glb back: every triangle's winding agrees with its vertex normals (faces out).
        const std::string glb = read_file(root + "/assets/models/" + shape + ".glb");
        REQUIRE(glb.size() > 28);
        CHECK(glb.compare(0, 4, "glTF") == 0);
        u32 json_len = 0;
        std::memcpy(&json_len, glb.data() + 12, 4);
        auto doc = parse(glb.substr(20, json_len));
        const usize bin = 20 + json_len + 8;
        const cook::JsonValue views = doc->root().get("bufferViews");
        const auto view_at = [&](u32 i) { return bin + static_cast<usize>(views.at(i).get("byteOffset").as_number()); };
        const usize n_idx = static_cast<usize>(doc->root().get("accessors").at(3).get("count").as_number());
        const auto f = [&](usize view, usize i) {
            float v = 0;
            std::memcpy(&v, glb.data() + view + i * 4, 4);
            return v;
        };
        u32 bad = 0;
        for (usize t3 = 0; t3 < n_idx; t3 += 3) {
            u32 id[3];
            for (int k = 0; k < 3; ++k) std::memcpy(&id[k], glb.data() + view_at(3) + (t3 + k) * 4, 4);
            float p[3][3], n[3] = {0, 0, 0};
            for (int k = 0; k < 3; ++k)
                for (int a = 0; a < 3; ++a) {
                    p[k][a] = f(view_at(0), id[k] * 3 + a);
                    n[a] += f(view_at(1), id[k] * 3 + a);
                }
            const float ux = p[1][0] - p[0][0], uy = p[1][1] - p[0][1], uz = p[1][2] - p[0][2];
            const float vx = p[2][0] - p[0][0], vy = p[2][1] - p[0][1], vz = p[2][2] - p[0][2];
            const float gx = uy * vz - uz * vy, gy = uz * vx - ux * vz, gz = ux * vy - uy * vx;
            if (gx * n[0] + gy * n[1] + gz * n[2] <= 0) ++bad;
        }
        CHECK(bad == 0);
    }
    auto cook = c.ok("cook_run");
    CHECK(cook.v().get("ok").as_bool());
    CHECK(listed(c, "models/ramp.glb"));
    CHECK(read_file(root + "/CREDITS.md").find("made with gen_mesh (sphere 2x1x3 m)") != std::string::npos);
    CHECK(c.fails("gen_mesh", R"({"shape":"teapot"})").find("'shape'") != std::string::npos);
    CHECK(c.fails("gen_mesh", R"({"size":[1,0,1]})").find("'size'") != std::string::npos);
    CHECK(c.fails("gen_mesh", R"({"save":"models/x.gltf"})").find(".glb") != std::string::npos);
}
