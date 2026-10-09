// mce_mcp_core in process (editor-mcp.md §6): the JSON writer, JSON-RPC/MCP protocol handling, every
// tool group against a copy of the scene_demo project, the FR-EDT-160 gate (an edit made through
// the tools saves the same bytes and hashes the same as the edit made through EditorScene
// commands, and as mce_player plays it), and a deterministic fuzz pass over the request path.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "cook_test_util.h"
#include "mce/audio/components.h"
#include "mce/editor/play_session.h"
#include "mce/input/ecs.h"
#include "mce/ecs/transform.h"
#include "mce/kit/components.h"
#include "mce/mcp/headless_run.h"
#include "mce/mcp/server.h"
#include "mce/physics/components.h"
#include "mce/platform/process.h"

using namespace mce;
using namespace mce::mcp;
using namespace cook_test;

namespace mcptest {

// A component with the field shapes the codec must handle: a read-only field, an entity
// reference, a string, a fixed array.
struct McpProbe {
    i32 hp = 10;
    f32 computed = 0.0f;
    Entity target;
    FixedString<16> label;
    math::vec2 points[3];
};
MCE_COMPONENT(McpProbe, hp, (computed, ReadOnly), target, label, points)

// A game component with runtime state: the game install below counts ticks into it.
struct McpCounter {
    u32 step = 1;
    u32 ticks = 0;  // runtime
    u32 jumps = 0;  // runtime: Jump presses seen (run_headless input)
    f32 moved = 0;  // runtime: sum of Move.x
};
MCE_COMPONENT(McpCounter, step, (ticks, ReadOnly, Transient), (jumps, ReadOnly, Transient), (moved, ReadOnly, Transient))

}  // namespace mcptest

namespace {

const std::string kDemoAssets = std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets";
constexpr const char* kBallGuid = "guid:5ce4e000-0000-4000-8000-000000000003";
constexpr const char* kDemoGuid = "5ce4e000-0000-4000-8000-000000000100";

std::string demo_project(const char* name) {
    const std::string root = fresh_dir(name);
    std::filesystem::copy(kDemoAssets, root + "/assets", std::filesystem::copy_options::recursive);
    return root;
}

StringView sv(const std::string& s) { return StringView(s.data(), s.size()); }

struct Parsed {
    cook::JsonDoc doc;
    cook::JsonValue root() const { return doc.root(); }
};

std::unique_ptr<Parsed> parse(const std::string& text) {
    auto p = std::make_unique<Parsed>();
    cook::JsonError err;
    REQUIRE_MESSAGE(p->doc.parse(sv(text), err), text);
    return p;
}

std::string str(cook::JsonValue v) { return v.is_string() ? std::string(v.as_string()) : std::string(); }

std::string compact(cook::JsonValue v) {
    cook::String s;
    cook::write_json(v, s, true);
    return std::string(s.view());
}

struct Client {
    Server server;
    std::vector<std::string> out;
    int next_id = 1;

    explicit Client(const ServerOptions& opt = {}) : server(opt) {
        server.set_sink([](StringView m, void* u) { static_cast<Client*>(u)->out.emplace_back(m); }, this);
    }

    // Sends one line; returns every message the server wrote for it.
    std::vector<std::string> send_all(const std::string& line) {
        out.clear();
        server.handle(sv(line));
        return out;
    }
    std::unique_ptr<Parsed> send(const std::string& line) {
        const std::vector<std::string> msgs = send_all(line);
        REQUIRE(!msgs.empty());
        return parse(msgs.back());
    }
    std::unique_ptr<Parsed> request(const std::string& method, const std::string& params = "{}") {
        return send(R"({"jsonrpc":"2.0","id":)" + std::to_string(next_id++) + R"(,"method":")" + method +
                    R"(","params":)" + params + "}");
    }
    void init() {
        auto r = request("initialize", R"({"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"1"}})");
        REQUIRE(r->root().get("result").is_object());
        CHECK(send_all(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty());
        CHECK(server.initialized());
    }

    struct Tool {
        bool error = true;
        std::string text;
        std::unique_ptr<Parsed> value;  // structuredContent, when it succeeded
        cook::JsonValue v() const { return value->root(); }
    };
    Tool tool(const std::string& name, const std::string& args = "{}") {
        auto r = request("tools/call", R"({"name":")" + name + R"(","arguments":)" + args + "}");
        const cook::JsonValue res = r->root().get("result");
        REQUIRE_MESSAGE(res.is_object(), compact(r->root()));
        Tool t;
        t.error = res.get("isError").as_bool();
        t.text = str(res.get("content").at(0).get("text"));
        if (!t.error) {
            t.value = parse(t.text);
            CHECK(compact(t.v()) == compact(res.get("structuredContent")));
        }
        return t;
    }
    Tool ok(const std::string& name, const std::string& args = "{}") {
        Tool t = tool(name, args);
        REQUIRE_MESSAGE(!t.error, (name + ": " + t.text));
        return t;
    }
    std::string fails(const std::string& name, const std::string& args = "{}") {
        Tool t = tool(name, args);
        CHECK_MESSAGE(t.error, (name + " should fail: " + t.text));
        return t.text;
    }
};

i64 error_code(const Parsed& p) {
    const cook::JsonValue e = p.root().get("error");
    return e.is_object() ? static_cast<i64>(e.get("code").as_number()) : 0;
}

}  // namespace

// ---- JSON writer -------------------------------------------------------------------------------

TEST_CASE("mcp json writer: nesting, commas, escapes, copies") {
    cook::String s;
    JsonWriter w(s);
    w.begin_object().key("a").num_i(-3).key("b").begin_array().str("x\"\n").boolean(true).null().num_u(7).end_array();
    w.key("c").begin_object().end_object().key("d").num_f(0.5).key("e").raw("[1,2]");
    cook::JsonDoc d;
    cook::JsonError err;
    REQUIRE(d.parse(R"({"z": [1, {"y": "q"}]})", err));
    w.key("f").copy(d.root().get("z"));
    w.end_object();
    CHECK(w.depth() == 0);
    CHECK(std::string(s.view()) == R"({"a":-3,"b":["x\"\n",true,null,7],"c":{},"d":0.5,"e":[1,2],"f":[1,{"y":"q"}]})");
}

// ---- protocol ----------------------------------------------------------------------------------

TEST_CASE("mcp protocol: JSON-RPC framing, initialize handshake, errors, lists") {
    Client c;
    // Malformed input never goes unanswered and never crashes.
    auto bad = c.send("{nope");
    CHECK(error_code(*bad) == kParseError);
    CHECK(bad->root().get("id").is_null());
    CHECK(error_code(*c.send("[1,2]")) == kInvalidRequest);
    CHECK(error_code(*c.send("42")) == kInvalidRequest);
    CHECK(error_code(*c.send(R"({"jsonrpc":"1.0","id":1,"method":"ping"})")) == kInvalidRequest);
    CHECK(error_code(*c.send(R"({"jsonrpc":"2.0","id":{},"method":"ping"})")) == kInvalidRequest);
    CHECK(error_code(*c.send(R"({"jsonrpc":"2.0","id":1})")) == kInvalidRequest);
    CHECK(c.send_all(R"({"jsonrpc":"2.0","id":1,"result":{}})").empty());  // a client response: ignored

    // ping works before initialize; everything else waits for it.
    CHECK(c.request("ping")->root().get("result").is_object());
    CHECK(error_code(*c.request("tools/list")) == kInvalidRequest);

    auto init = c.request("initialize", R"({"protocolVersion":"2024-11-05"})");
    CHECK(str(init->root().get("result").get("protocolVersion")) == "2024-11-05");  // a supported version is echoed
    auto init2 = c.request("initialize", R"({"protocolVersion":"1999-01-01"})");
    CHECK(str(init2->root().get("result").get("protocolVersion")) == kProtocolVersion);
    CHECK(str(init2->root().get("result").get("serverInfo").get("name")) == "mce_mcp");
    CHECK(init2->root().get("result").get("capabilities").get("tools").is_object());
    CHECK(c.send_all(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty());
    CHECK(c.send_all(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":3}})").empty());

    // String ids are echoed as strings.
    auto sid = c.send(R"({"jsonrpc":"2.0","id":"abc","method":"ping"})");
    CHECK(str(sid->root().get("id")) == "abc");

    auto tools = c.request("tools/list");
    const cook::JsonValue list = tools->root().get("result").get("tools");
    REQUIRE(list.is_array());
    CHECK(list.size() == 24);
    for (u32 i = 0; i < list.size(); ++i) {
        CHECK(list.at(i).get("name").is_string());
        CHECK(str(list.at(i).get("inputSchema").get("type")) == "object");
    }

    CHECK(error_code(*c.request("nope/nope")) == kMethodNotFound);
    CHECK(error_code(*c.request("tools/call", R"({"name":"no_such_tool"})")) == kInvalidParams);
    CHECK(error_code(*c.request("tools/call", R"({"name":"scene_info","arguments":[]})")) == kInvalidParams);
    CHECK(error_code(*c.request("tools/call", "[]")) == kInvalidParams);

    auto res = c.request("resources/list");
    CHECK(res->root().get("result").get("resources").size() == 3);
    auto schema = c.request("resources/read", R"({"uri":"mce://refl/schema"})");
    const std::string schema_text = str(schema->root().get("result").get("contents").at(0).get("text"));
    CHECK(schema_text.find("\"Collider2D\"") != std::string::npos);
    auto scene = c.request("resources/read", R"({"uri":"mce://scene/current"})");
    CHECK(str(scene->root().get("result").get("contents").at(0).get("text")).find("mce-scene") != std::string::npos);
    auto logr = c.request("resources/read", R"({"uri":"mce://log"})");
    CHECK(str(logr->root().get("result").get("contents").at(0).get("mimeType")) == "text/plain");
    CHECK(error_code(*c.request("resources/read", R"({"uri":"mce://nothing"})")) == kResourceNotFound);
}

// ---- tools -------------------------------------------------------------------------------------

TEST_CASE("mcp tools: discover components from mce_refl") {
    Client c;
    c.init();
    auto list = c.ok("refl_list_components");
    std::vector<std::string> names;
    for (u32 i = 0; i < list.v().get("components").size(); ++i) names.push_back(str(list.v().get("components").at(i).get("name")));
    CHECK(std::find(names.begin(), names.end(), "Sprite") != names.end());
    CHECK(std::find(names.begin(), names.end(), "LocalTransform") != names.end());
    CHECK(std::is_sorted(names.begin(), names.end()));

    auto col = c.ok("refl_describe_component", R"({"name":"Collider2D"})");
    const cook::JsonValue fields = col.v().get("fields");
    REQUIRE(fields.size() == 6);
    const cook::JsonValue shape = fields.at(0);
    CHECK(str(shape.get("kind")) == "struct");
    CHECK(str(shape.get("type")) == "ColliderShape");
    const cook::JsonValue type = shape.get("fields").at(0);
    CHECK(str(type.get("type")) == "ShapeType");
    CHECK(compact(type.get("values")).find("\"Circle\"") != std::string::npos);
    CHECK(fields.at(2).get("max").as_number() == doctest::Approx(2.0));

    auto sprite = c.ok("refl_describe_component", R"({"name":"Sprite"})");
    CHECK(str(sprite.v().get("fields").at(0).get("type")) == "Texture");
    auto probe = c.ok("refl_describe_component", R"({"name":"McpProbe"})");
    CHECK(probe.v().get("fields").at(1).get("read_only").as_bool());
    CHECK(probe.v().get("fields").at(4).get("count").as_number() == 3);

    CHECK(c.fails("refl_describe_component", R"({"name":"Nope"})").find("unknown component type") != std::string::npos);
    CHECK(c.fails("refl_describe_component").find("'name' must be a string") != std::string::npos);
    // Project-bound tools say what is missing.
    CHECK(c.fails("asset_list").find("--project") != std::string::npos);
    CHECK(c.fails("run_headless").find("--project") != std::string::npos);
}

TEST_CASE("mcp tools: read and edit through the funnel; values are validated before anything changes") {
    const std::string root = demo_project("mcp_edit");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();

    auto info = c.ok("scene_info");
    CHECK(str(info.v().get("path")) == "scenes/demo.mcscene");
    CHECK(info.v().get("entities").as_number() == 6);
    CHECK_FALSE(info.v().get("dirty").as_bool());

    auto ents = c.ok("scene_list_entities", R"({"component":"RigidBody2D"})");
    CHECK(ents.v().get("entities").size() == 4);
    auto crate = c.ok("scene_get_entity", R"({"entity":3})");
    CHECK(str(crate.v().get("name")) == "Crate A");
    CHECK(compact(crate.v().get("components").get("LocalTransform")) == R"({"position":[-2,2,0]})");
    CHECK(c.fails("scene_get_entity", R"({"entity":99})").find("no entity with id 99") != std::string::npos);

    auto textures = c.ok("asset_list", R"({"type":"Texture"})");
    CHECK(textures.v().get("assets").size() == 3);
    CHECK(str(textures.v().get("assets").at(0).get("guid")).rfind("guid:", 0) == 0);

    // Create with components: one call, sorted by the document.
    auto made = c.ok("entity_create", std::string(R"({"name":"Pickup","components":{"Sprite":{"texture":")") + kBallGuid +
                                          R"(","size":[0.5,0.5]},"Collider2D":{"is_sensor":true,"shape":{"type":"Circle","radius":0.25}}}})");
    const int pickup = static_cast<int>(made.v().get("id").as_number());
    CHECK(pickup == 7);
    auto got = c.ok("scene_get_entity", R"({"entity":7})");
    CHECK(compact(got.v().get("components").get("Collider2D")) == R"({"is_sensor":true,"shape":{"radius":0.25,"type":"Circle"}})");
    CHECK(got.v().get("components").get("LocalTransform").is_object());

    // A bad value anywhere: nothing is created.
    const usize before = c.server.session().scene().doc().size();
    CHECK(c.fails("entity_create", R"({"name":"X","components":{"Sprite":{"size":[1]}}})").find("expected an array of 2 numbers") !=
          std::string::npos);
    CHECK(c.fails("entity_create", R"({"name":"X","components":{"Bogus":{}}})").find("unknown component type 'Bogus'") !=
          std::string::npos);
    CHECK(c.fails("entity_create", R"({"name":"X","components":{"Sprite":{"nope":1}}})").find("unknown field 'nope'") !=
          std::string::npos);
    CHECK(c.server.session().scene().doc().size() == before);

    // component_set: object patch, property path, vector lane, type errors that name the path.
    CHECK(c.ok("component_set", R"({"entity":7,"component":"LocalTransform","path":"position","value":[3,1,0]})").v().get("changed").as_bool());
    c.ok("component_set", R"({"entity":7,"component":"LocalTransform","path":"position.y","value":1.5})");
    auto lt = c.ok("component_set", R"({"entity":7,"component":"Sprite","value":{"order":4,"flip_x":true}})");
    CHECK(compact(lt.v().get("value")).find("\"order\":4") != std::string::npos);
    CHECK(compact(lt.v().get("value")).find("\"size\":[0.5,0.5]") != std::string::npos);  // patch keeps the rest
    CHECK_FALSE(c.ok("component_set", R"({"entity":7,"component":"Sprite","path":"order","value":4})").v().get("changed").as_bool());
    CHECK(c.server.session().scene().get<LocalTransform>(7)->position.y == 1.5f);
    std::string e = c.fails("component_set", R"({"entity":7,"component":"LocalTransform","path":"position.y","value":"x"})");
    CHECK(e.find("expected a number") != std::string::npos);
    CHECK(e.find("(at position.y)") != std::string::npos);
    e = c.fails("component_set", R"({"entity":7,"component":"Collider2D","value":{"shape":{"type":"Hexagon"}}})");
    CHECK(e.find("unknown enumerator (at shape.type)") != std::string::npos);
    CHECK(c.fails("component_set", R"({"entity":7,"component":"Sprite","path":"nope","value":1})").find("no such property") != std::string::npos);
    CHECK(c.fails("component_set", R"({"entity":7,"component":"RigidBody2D","value":{}})").find("use component_add") != std::string::npos);
    CHECK(c.fails("component_set", R"({"entity":7,"component":"Sprite","path":"layer","value":3.5})").find("integer") != std::string::npos);

    // Probe: read-only fields, entity references, strings, arrays.
    c.ok("component_add", R"({"entity":7,"component":"McpProbe","value":{"hp":3}})");
    CHECK(c.fails("component_add", R"({"entity":7,"component":"McpProbe"})").find("already has") != std::string::npos);
    CHECK(c.fails("component_set", R"({"entity":7,"component":"McpProbe","value":{"computed":1}})").find("read-only") != std::string::npos);
    CHECK(c.fails("component_set", R"({"entity":7,"component":"McpProbe","path":"computed","value":1})").find("read-only") != std::string::npos);
    c.ok("component_set", R"({"entity":7,"component":"McpProbe","path":"target","value":"$3"})");
    CHECK(c.server.session().scene().get<mcptest::McpProbe>(7)->target == editor::doc_entity_ref(3));
    CHECK(c.fails("component_set", R"({"entity":7,"component":"McpProbe","path":"target","value":"$99"})").find("$<entity id>") != std::string::npos);
    CHECK(c.fails("component_set", R"({"entity":7,"component":"McpProbe","path":"target","value":"$0"})").find("no entity") != std::string::npos);
    CHECK(c.server.session().scene().get<mcptest::McpProbe>(7)->target == editor::doc_entity_ref(3));  // unchanged
    c.ok("component_set", R"({"entity":7,"component":"McpProbe","value":{"label":"hi","points":[[1,2],[3,4],[5,6]]}})");
    c.ok("component_set", R"({"entity":7,"component":"McpProbe","path":"points[1].x","value":9})");
    auto probe = c.ok("scene_get_entity", R"({"entity":7})");
    CHECK(compact(probe.v().get("components").get("McpProbe")) ==
          R"({"hp":3,"label":"hi","points":[[1,2],[9,4],[5,6]],"target":"$3"})");
    CHECK(c.fails("component_set", R"({"entity":7,"component":"McpProbe","path":"label","value":"this is far too long for it"})")
              .find("capacity") != std::string::npos);
    c.ok("component_set", R"({"entity":7,"component":"McpProbe","path":"target","value":null})");
    CHECK(c.server.session().scene().get<mcptest::McpProbe>(7)->target.is_null());
    CHECK(c.server.session().scene().verify_world());  // every edit was mirrored into the preview world
    c.ok("component_remove", R"({"entity":7,"component":"McpProbe"})");
    CHECK(c.fails("component_remove", R"({"entity":7,"component":"McpProbe"})").find("does not have") != std::string::npos);

    // Duplicate and delete.
    auto dup = c.ok("entity_duplicate", R"({"entity":3})");
    REQUIRE(dup.v().get("ids").size() == 1);
    const int copy = static_cast<int>(dup.v().get("ids").at(0).as_number());
    CHECK(str(c.ok("scene_get_entity", "{\"entity\":" + std::to_string(copy) + "}").v().get("name")) == "Crate A");
    c.ok("entity_delete", "{\"entity\":" + std::to_string(copy) + "}");
    CHECK(c.server.session().scene().doc().find(static_cast<editor::EntityId>(copy)) == nullptr);
    CHECK(c.server.session().scene().verify_world());
    CHECK(c.ok("scene_info").v().get("dirty").as_bool());
}

TEST_CASE("mcp tools: scene_undo / scene_redo step through tools/call edits (FR-EDT-040)") {
    const std::string root = demo_project("mcp_undo");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    const auto text = [&] {
        cook::String t;
        c.server.session().scene().serialize(t);
        return std::string(t.view());
    };
    const std::string original = text();
    CHECK(c.fails("scene_undo").find("nothing to undo") != std::string::npos);

    // entity_create with components is one call, so one step.
    c.ok("entity_create", R"({"name":"Pickup","components":{"Sprite":{"size":[0.5,0.5]}}})");
    const std::string created = text();
    c.ok("component_set", R"({"entity":3,"component":"LocalTransform","path":"position.x","value":9})");
    c.ok("entity_delete", R"({"entity":4})");
    c.ok("scene_info");  // reads are no step
    const std::string last = text();

    auto u = c.ok("scene_undo");
    CHECK(str(u.v().get("undone")) == "entity_delete");
    CHECK(u.v().get("can_redo").as_bool());
    c.ok("scene_undo");
    CHECK(text() == created);
    auto u3 = c.ok("scene_undo");
    CHECK(str(u3.v().get("undone")) == "entity_create");
    CHECK_FALSE(u3.v().get("can_undo").as_bool());
    CHECK(text() == original);
    CHECK(c.server.session().scene().verify_world());

    c.ok("scene_redo");
    c.ok("scene_redo");
    auto r = c.ok("scene_redo");
    CHECK(str(r.v().get("redone")) == "entity_delete");
    CHECK(text() == last);
    CHECK(c.fails("scene_redo").find("nothing to redo") != std::string::npos);

    // A failed call leaves no step behind.
    const usize steps = c.server.session().scene().undo_count();
    c.fails("entity_create", R"({"name":"X","components":{"Bogus":{}}})");
    CHECK(c.server.session().scene().undo_count() == steps);
}

TEST_CASE("mcp tools: persistence stays inside the project and never clobbers silently") {
    const std::string root = demo_project("mcp_persist");
    Client c;
    REQUIRE(c.server.open_project(sv(root)));
    c.init();
    for (const char* bad : {"../demo.mcscene", "/etc/passwd.mcscene", "scenes/../../x.mcscene", "scenes\\\\demo.mcscene",
                            "scenes/demo.txt", "C:/x.mcscene", "scenes//demo.mcscene", ""})
        CHECK(c.fails("scene_open", std::string(R"({"path":")") + bad + "\"}").find("invalid scene path") != std::string::npos);
    CHECK(c.fails("scene_open", R"({"path":"scenes/missing.mcscene"})").find("no such scene") != std::string::npos);
    CHECK(c.fails("scene_save").find("never saved") != std::string::npos);

    c.ok("scene_open", R"({"path":"scenes/demo.mcscene"})");
    c.ok("entity_create", R"({"name":"Marker"})");
    CHECK(c.fails("scene_open", R"({"path":"scenes/demo.mcscene"})").find("unsaved changes") != std::string::npos);
    CHECK(c.fails("scene_new", R"({"name":"x"})").find("unsaved changes") != std::string::npos);
    CHECK(c.fails("run_headless").find("unsaved changes") != std::string::npos);

    // Changed on disk behind the server's back: refused until forced.
    const std::string file = root + "/assets/scenes/demo.mcscene";
    write_file(file, read_file(file) + "\n");
    CHECK(c.fails("scene_save").find("changed on disk") != std::string::npos);
    c.ok("scene_save", R"({"force":true})");
    CHECK(read_file(file).find("\"Marker\"") != std::string::npos);
    c.ok("component_set", R"({"entity":7,"component":"LocalTransform","path":"position.x","value":2})");
    c.ok("scene_save");  // our own last write is not a conflict

    // Save As: a new file (new GUID), never over another existing file unless forced.
    auto as = c.ok("scene_save", R"({"path":"scenes/copy.mcscene"})");
    CHECK(str(as.v().get("path")) == "scenes/copy.mcscene");
    CHECK(str(as.v().get("guid")) != kDemoGuid);
    CHECK(std::filesystem::exists(root + "/assets/scenes/copy.mcscene"));
    CHECK(c.fails("scene_save", R"({"path":"scenes/demo.mcscene"})").find("already exists") != std::string::npos);
    CHECK(c.ok("asset_list", R"({"type":"Scene"})").v().get("assets").size() == 2);

    c.ok("scene_new", R"({"name":"fresh"})");
    CHECK(c.ok("scene_info").v().get("path").is_null());
    c.ok("scene_save", R"({"path":"levels/fresh.mcscene"})");
    CHECK(std::filesystem::exists(root + "/assets/levels/fresh.mcscene"));

    // Cook errors come back with file and line.
    write_file(root + "/assets/scenes/broken.mcscene", "{\n \"format\": \"mce-scene\",\n \"bogus\": ,\n}");
    auto cook = c.ok("cook_run");
    CHECK_FALSE(cook.v().get("ok").as_bool());
    auto errs = c.ok("cook_errors");
    const std::string all = compact(errs.v().get("diagnostics"));
    CHECK(all.find("broken.mcscene") != std::string::npos);
    CHECK(errs.v().get("errors").as_number() >= 1);
    CHECK(c.fails("run_headless").find("cook failed") != std::string::npos);
    std::filesystem::remove(root + "/assets/scenes/broken.mcscene");
    CHECK(c.ok("cook_run", R"({"pack":true})").v().get("ok").as_bool());
    CHECK(std::filesystem::exists(root + "/assets.mce"));
}

namespace {

void count_ticks(Frame& f) {
    const input::InputState& in = f.resource<input::InputState>();
    f.query<mcptest::McpCounter&>().each([&](mcptest::McpCounter& c) {
        c.ticks += c.step;
        if (in.jump_pressed) ++c.jumps;
        c.moved += in.move.x;
    });
}

ResultV counter_game_install(World& w) {
    w.register_components<mcptest::McpCounter>();
    w.add_system(Phase::FixedUpdate_Pre, "mcp_test_count", &count_ticks);
    return ok();
}

}  // namespace

TEST_CASE("mcp tools: run_headless runs the project's game systems and inspects the final state") {
    const std::string root = demo_project("mcp_game");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    const std::string id = std::to_string(static_cast<int>(
        c.ok("entity_create", R"({"name":"Counter","components":{"McpCounter":{"step":3}}})").v().get("id").as_number()));
    c.ok("scene_save");
    const std::string args = R"({"ticks":20,"log_lines":0,"inspect":{"component":"McpCounter"}})";

    // mce_mcp alone: engine systems only, the counter never moves.
    auto bare = c.ok("run_headless", args);
    cook::JsonValue e = bare.v().get("inspect").get("entities").at(0);
    CHECK(e.get("id").as_number() == std::stod(id));
    CHECK(str(e.get("name")) == "Counter");
    CHECK(e.get("components").get("McpCounter").get("ticks").as_number() == 0);  // Transient fields are reported

    // A project server (<game>_mcp) registers the game's install(): its systems run in the run.
    editor::set_play_game_install(&counter_game_install);
    auto game = c.ok("run_headless", args);
    editor::set_play_game_install(nullptr);
    e = game.v().get("inspect").get("entities").at(0);
    CHECK(e.get("components").get("McpCounter").get("ticks").as_number() == 60);
    CHECK(e.get("components").get("McpCounter").get("step").as_number() == 3);
    CHECK(str(game.v().get("state_hash")) != str(bare.v().get("state_hash")));

    // By id, with chosen components; a scene entity's own components by default.
    auto by_id = c.ok("run_headless", "{\"ticks\":1,\"inspect\":{\"entities\":[" + id + R"(],"show":["LocalTransform"]}})");
    const cook::JsonValue comps = by_id.v().get("inspect").get("entities").at(0).get("components");
    CHECK(comps.get("LocalTransform").is_object());
    CHECK_FALSE(comps.get("McpCounter").valid());
    // Scripted input: presses and holds reach the game's systems through the InputState resource.
    editor::set_play_game_install(&counter_game_install);
    auto driven = c.ok("run_headless", R"({"ticks":20,"log_lines":0,"input":"3-10 Move=1,0\n5 Jump\n12-14 Jump","inspect":{"component":"McpCounter"}})");
    e = driven.v().get("inspect").get("entities").at(0);
    CHECK(driven.v().get("input_segments").as_number() == 3);
    CHECK(e.get("components").get("McpCounter").get("jumps").as_number() == 2);
    CHECK(e.get("components").get("McpCounter").get("moved").as_number() == 8);
    CHECK(str(driven.v().get("state_hash")) != str(game.v().get("state_hash")));
    auto segs = c.ok("run_headless", R"({"ticks":20,"log_lines":0,"input":[{"from":2,"to":4,"actions":{"Move":[1,0],"Jump":true}},{"from":9,"actions":{"Jump":1}}],"inspect":{"component":"McpCounter"}})");
    e = segs.v().get("inspect").get("entities").at(0);
    CHECK(e.get("components").get("McpCounter").get("jumps").as_number() == 2);
    CHECK(e.get("components").get("McpCounter").get("moved").as_number() == 3);
    editor::set_play_game_install(nullptr);
    CHECK(c.fails("run_headless", R"({"input":"x Jump"})").find("line 1") != std::string::npos);
    CHECK(c.fails("run_headless", R"({"input":[{"to":3,"actions":{"Jump":true}}]})").find("from") != std::string::npos);
    CHECK(c.fails("run_headless", R"({"inspect":{"entities":[9999]}})").find("no entity") != std::string::npos);
    CHECK(c.fails("run_headless", R"({"inspect":{"component":"Nope"}})").find("unknown component") != std::string::npos);
    CHECK(c.fails("run_headless", R"({"inspect":{}})").find("give 'entities'") != std::string::npos);
}

TEST_CASE("mcp tools: cook_run adopt gives new source files a sidecar") {
    const std::string root = demo_project("mcp_adopt");
    std::filesystem::copy_file(root + "/assets/textures/ball.png", root + "/assets/textures/new_ball.png");
    Client c;
    REQUIRE(c.server.open_project(sv(root)));
    c.init();
    auto cook = c.ok("cook_run");
    CHECK_FALSE(cook.v().get("ok").as_bool());
    const std::string diags = compact(cook.v().get("diagnostics"));
    CHECK(diags.find("new_ball.png") != std::string::npos);
    CHECK(diags.find("adopt") != std::string::npos);
    CHECK(c.ok("cook_run", R"({"adopt":true})").v().get("ok").as_bool());
    CHECK(std::filesystem::exists(root + "/assets/textures/new_ball.png.mceasset"));
    CHECK(compact(c.ok("asset_list", R"({"type":"Texture"})").v()).find("textures/new_ball.png") != std::string::npos);
    CHECK(c.ok("cook_run").v().get("ok").as_bool());  // registered now: a plain cook is clean
}

TEST_CASE("mcp tools: scene_clear empties the scene in one undo step") {
    const std::string root = demo_project("mcp_clear");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    const auto count = [&] { return c.ok("scene_info").v().get("entities").as_number(); };
    const double before = count();
    REQUIRE(before > 1);
    auto r = c.ok("scene_clear");
    CHECK(r.v().get("deleted").as_number() == before);
    CHECK(count() == 0);
    CHECK(c.server.session().scene().verify_world());
    CHECK(c.ok("scene_info").v().get("dirty").as_bool());
    // The scene keeps its identity: the .mceproj still names it after a save.
    CHECK(str(c.ok("scene_info").v().get("guid")) == kDemoGuid);
    auto u = c.ok("scene_undo");
    CHECK(str(u.v().get("undone")) == "scene_clear");
    CHECK(count() == before);
    CHECK(c.server.session().scene().verify_world());
    c.ok("scene_redo");
    CHECK(count() == 0);
    CHECK(c.ok("scene_clear").v().get("deleted").as_number() == 0);  // already empty: no error
}

TEST_CASE("mcp tools: asset_set_import_settings rewrites a sidecar's settings") {
    const std::string root = demo_project("mcp_import_settings");
    Client c;
    REQUIRE(c.server.open_project(sv(root)));
    c.init();
    CHECK(c.fails("asset_set_import_settings", R"({"path":"textures/ball.png"})").find("'settings'") != std::string::npos);
    CHECK(c.fails("asset_set_import_settings", R"({"settings":{}})").find("one of 'path'") != std::string::npos);
    CHECK(c.fails("asset_set_import_settings", R"({"path":"textures/nope.png","settings":{}})").find("no registered asset") != std::string::npos);
    CHECK(c.fails("asset_set_import_settings", R"({"guid":"guid:5ce4e000-0000-4000-8000-0000000000ff","settings":{}})").find("no registered asset") != std::string::npos);

    auto r = c.ok("asset_set_import_settings", R"({"path":"textures/ball.png","settings":{"srgb":false,"mips":false}})");
    CHECK(str(r.v().get("guid")) == kBallGuid);
    CHECK_FALSE(r.v().get("settings").get("srgb").as_bool());
    std::string sidecar = read_file(root + "/assets/textures/ball.png.mceasset");
    auto p = parse(sidecar);
    CHECK(str(p->root().get("guid")) == std::string(kBallGuid).substr(5));  // the other members survived
    CHECK(str(p->root().get("importer")) == "texture");
    CHECK_FALSE(p->root().get("settings").get("srgb").as_bool());
    CHECK_FALSE(p->root().get("settings").get("mips").as_bool());
    // By guid, and back to the defaults: the cook takes the change (and rejects an unknown key).
    CHECK(c.ok("asset_set_import_settings", std::string("{\"guid\":\"") + kBallGuid + R"(","settings":{"srgb":true}})").v().get("path").as_string() == "textures/ball.png");
    CHECK(c.ok("cook_run").v().get("ok").as_bool());
    c.ok("asset_set_import_settings", R"({"path":"textures/ball.png","settings":{"bogus":1}})");
    auto cook = c.ok("cook_run");
    CHECK_FALSE(cook.v().get("ok").as_bool());
    CHECK(compact(cook.v().get("diagnostics")).find("bogus") != std::string::npos);
}

TEST_CASE("mcp tools: a relative --project path saves where it says") {
    const std::string root = demo_project("mcp_relative");
    const std::string rel = std::filesystem::relative(root).generic_string();
    REQUIRE(rel.front() != '/');
    Client c;
    REQUIRE(c.server.open_project(sv(rel), "scenes/demo.mcscene"));
    c.init();
    c.ok("scene_save", R"({"path":"scenes/copy.mcscene"})");
    CHECK(std::filesystem::exists(root + "/assets/scenes/copy.mcscene"));
    CHECK_FALSE(std::filesystem::exists(root + "/assets/" + rel));  // once: assets/<rel>/assets/scenes/copy.mcscene
}

TEST_CASE("mcp tools: console list/exec, cheats refused by default") {
    Client c;
    c.init();
    auto list = c.ok("console_list", R"({"prefix":"time_"})");
    REQUIRE(list.v().get("convars").size() >= 1);
    CHECK(str(list.v().get("convars").at(0).get("name")).rfind("time_", 0) == 0);
    CHECK(c.fails("console_exec", R"({"line":"time_scale 2"})").find("--allow-cheats") != std::string::npos);
    c.ok("console_exec", R"({"line":"time_scale"})");  // reading is fine
    CHECK(c.fails("console_exec", R"({"line":"no_such_var 1"})").find("unknown name") != std::string::npos);
    CHECK(c.fails("console_exec", R"({"line":""})").find("one console line") != std::string::npos);

    Client cheats(ServerOptions{true});
    cheats.init();
    cheats.ok("console_exec", R"({"line":"time_scale 1"})");
}

// ---- the FR-EDT-160 gate -----------------------------------------------------------------------

TEST_CASE("FR-EDT-160 gate: MCP edit == SceneCommand edit (same .mcscene bytes, same run_headless hash, same as mce_player)") {
    // Through the MCP tools.
    const std::string a = demo_project("mcp_gate_tools");
    std::string hash_a;
    {
        Client c;
        REQUIRE(c.server.open_project(sv(a), "scenes/demo.mcscene"));
        c.init();
        c.ok("entity_create", std::string(R"({"name":"Pickup","components":{"Sprite":{"texture":")") + kBallGuid +
                                  R"(","size":[0.5,0.5],"order":2},"Collider2D":{"is_sensor":true,"shape":{"type":"Circle","radius":0.25}}}})");
        c.ok("component_set", R"({"entity":7,"component":"LocalTransform","path":"position","value":[3,1,0]})");
        auto dup = c.ok("entity_duplicate", R"({"entity":3})");
        const std::string id = std::to_string(static_cast<int>(dup.v().get("ids").at(0).as_number()));
        c.ok("component_set", "{\"entity\":" + id + R"(,"component":"LocalTransform","path":"position.x","value":-1})");
        c.ok("component_set", R"({"entity":6,"component":"TextLabel","value":{"text":"EDITED BY MCP"}})");
        c.ok("scene_save");
        auto run = c.ok("run_headless", R"({"ticks":300})");
        CHECK(run.v().get("ticks").as_number() == 300);
        CHECK(run.v().get("entities").as_number() == 8);
        CHECK(run.v().get("metrics").get("entities").as_number() == 8);
        hash_a = str(run.v().get("state_hash"));
        CHECK(hash_a.size() == 16);
        // Same scene + ticks: same hash.
        CHECK(str(c.ok("run_headless", R"({"ticks":300})").v().get("state_hash")) == hash_a);
    }

    // The same edit through EditorScene commands.
    const std::string b = demo_project("mcp_gate_commands");
    editor::EditorSession session;
    REQUIRE(session.open_project(sv(b)));
    REQUIRE(session.open_scene("scenes/demo.mcscene"));
    editor::EditorScene& s = session.scene();
    const editor::EntityId pickup = s.create_entity("Pickup");
    s.add_component(pickup, refl::type_of<kit::Sprite>());
    kit::Sprite sp;
    sp.texture.guid = asset::parse_guid(StringView(kBallGuid).substr(5)).value();
    sp.size = {0.5f, 0.5f};
    sp.order = 2;
    s.set(pickup, sp);
    s.add_component(pickup, refl::type_of<physics::Collider2D>());
    physics::Collider2D col;
    col.is_sensor = true;
    col.shape.type = physics::ShapeType::Circle;
    col.shape.radius = 0.25f;
    s.set(pickup, col);
    LocalTransform lt = *s.get<LocalTransform>(pickup);
    lt.position = {3, 1, 0};
    s.set(pickup, lt);
    Array<editor::EntityId> dup;
    const editor::EntityId crate[] = {3};
    REQUIRE(s.duplicate_entities(Span<const editor::EntityId>(crate, 1), &dup));
    LocalTransform dt = *s.get<LocalTransform>(dup[0]);
    dt.position.x = -1;
    s.set(dup[0], dt);
    kit::TextLabel tl = *s.get<kit::TextLabel>(6);
    tl.text.assign("EDITED BY MCP");
    s.set(6, tl);
    REQUIRE(session.save_scene());

    const std::string bytes_a = read_file(a + "/assets/scenes/demo.mcscene");
    CHECK(bytes_a == read_file(b + "/assets/scenes/demo.mcscene"));
    CHECK(bytes_a.find("\"Pickup\"") != std::string::npos);

    REQUIRE(session.cook().ok());
    const std::string cooked_b = b + "/cooked";
    Result<HeadlessRun> run_b = run_headless(sv(cooked_b), s.doc().guid(), 300);
    REQUIRE(run_b);
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(run_b.value().state_hash));
    CHECK(hash_a == hex);

    // And mce_player, the determinism harness's host, plays the MCP-saved scene to the same hash.
    const std::string cooked_a = a + "/cooked";
    const StringView args[] = {MCE_PLAYER_EXE, "-headless", "1", "-ticks", "300", "-pack", sv(cooked_a), "-scene", kDemoGuid};
    platform::Process player;
    REQUIRE(player.start(Span<const StringView>(args, 9), true));
    Array<char> out;
    const i32 code = player.wait(&out);
    const std::string text(out.data(), out.size());
    CHECK_MESSAGE(code == 0, text);
    CHECK_MESSAGE(text.find("300 ticks, 8 entities, state_hash " + hash_a) != std::string::npos, text);
}

// ---- fuzz --------------------------------------------------------------------------------------

TEST_CASE("mcp fuzz: mutated and random requests always get well-formed answers") {
    const std::vector<std::string> corpus = {
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})",
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
        R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})",
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"entity_create","arguments":{"name":"A","components":{"Sprite":{"size":[2,3],"tint":[1,0,0,1]},"Collider2D":{"shape":{"type":"Circle","vertices":[[0,0],[1,0],[0,1],[0,0],[0,0],[0,0],[0,0],[0,0]]}}}}}})",
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"component_set","arguments":{"entity":1,"component":"LocalTransform","path":"rotation.w","value":1}}})",
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"component_set","arguments":{"entity":1,"component":"Sprite","value":{"texture":"guid:5ce4e000-0000-4000-8000-000000000003","layer":-3}}}})",
        R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"scene_get_entity","arguments":{"entity":1}}})",
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"refl_describe_component","arguments":{"name":"Collider2D"}}})",
        R"({"jsonrpc":"2.0","id":8,"method":"resources/read","params":{"uri":"mce://scene/current"}})",
        R"({"jsonrpc":"2.0","id":"x","method":"tools/call","params":{"name":"entity_duplicate","arguments":{"entity":1},"_meta":{"progressToken":7}}})",
        R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"component_add","arguments":{"entity":1,"component":"McpProbe","value":{"target":"$1","points":[[1,2],[3,4],[5,6]],"label":"\u00e9\n"}}}})",
        R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"scene_list_entities","arguments":{}}})",
    };
    std::mt19937 rng(0x4d4350u);
    const char alphabet[] = "{}[]\":,\\0123456789.-+eEtrufalsn \t$/abcxyz\x01\xff";
    Client c;
    usize answered = 0;
    for (int iter = 0; iter < 4000; ++iter) {
        std::string line;
        if (iter % 10 == 9) {  // pure noise
            const usize n = rng() % 200;
            for (usize i = 0; i < n; ++i) line += static_cast<char>(rng() % 256);
        } else {
            line = corpus[rng() % corpus.size()];
            const int edits = 1 + static_cast<int>(rng() % 6);
            for (int k = 0; k < edits && !line.empty(); ++k) {
                const usize at = rng() % line.size();
                switch (rng() % 5) {
                    case 0: line[at] = alphabet[rng() % (sizeof(alphabet) - 1)]; break;
                    case 1: line.erase(at, 1 + rng() % 8); break;
                    case 2: line.insert(at, 1, alphabet[rng() % (sizeof(alphabet) - 1)]); break;
                    case 3: line = line.substr(0, at); break;
                    default: line.insert(at, line.substr(at, rng() % 40)); break;
                }
            }
        }
        for (const std::string& m : c.send_all(line)) {
            cook::JsonDoc d;
            cook::JsonError err;
            REQUIRE_MESSAGE(d.parse(sv(m), err), m);
            REQUIRE(d.root().is_object());
            CHECK(str(d.root().get("jsonrpc")) == "2.0");
            ++answered;
        }
    }
    CHECK(answered > 1000);
    CHECK(c.server.session().scene().verify_world());
    // Deep nesting is refused by the codec's depth limit, not by the stack.
    const std::string deep = std::string(100000, '[') + std::string(100000, ']');
    CHECK(error_code(*c.send(deep)) == kParseError);
}
