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
#include <string_view>
#include <vector>

#include "cook_test_util.h"
#include "mce/convar.h"
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

// Joins pieces into one string (tool arguments built from variables).
std::string cat(std::initializer_list<std::string_view> parts) {
    std::string out;
    for (std::string_view p : parts) out += p;
    return out;
}

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
    CHECK(list.size() >= 37);  // a lower bound: tool groups land in parallel (tools.h)
    for (u32 i = 0; i < list.size(); ++i) {
        CHECK(list.at(i).get("name").is_string());
        CHECK(str(list.at(i).get("inputSchema").get("type")) == "object");
    }

    CHECK(error_code(*c.request("nope/nope")) == kMethodNotFound);
    CHECK(error_code(*c.request("tools/call", R"({"name":"no_such_tool"})")) == kInvalidParams);
    CHECK(error_code(*c.request("tools/call", R"({"name":"scene_info","arguments":[]})")) == kInvalidParams);
    CHECK(error_code(*c.request("tools/call", "[]")) == kInvalidParams);

    auto res = c.request("resources/list");
    CHECK(res->root().get("result").get("resources").size() >= 4);
    auto asset_schema = c.request("resources/read", R"({"uri":"mce://assets/schema"})");
    CHECK(str(asset_schema->root().get("result").get("contents").at(0).get("text")).find("mce-particles") != std::string::npos);
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

    // Reflected value types describe the same way: the runtime UI's anchors (M-MOBILE 11b).
    auto anchors = c.ok("refl_describe_component", R"({"name":"UiAnchors"})");
    const cook::JsonValue af = anchors.v().get("fields");
    REQUIRE(af.size() == 6);
    CHECK(str(af.at(5).get("type")) == "UiAnchorTo");
    CHECK(compact(af.at(5).get("values")).find("\"SafeArea\"") != std::string::npos);
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
    // By name, when the name is unique.
    auto by_name = c.ok("scene_get_entity", R"({"entity":"Crate A"})");
    CHECK(by_name.v().get("id").as_number() == 3);
    CHECK(c.fails("scene_get_entity", R"({"entity":"Nobody"})").find("no entity named 'Nobody'") != std::string::npos);
    CHECK(c.fails("scene_get_entity", R"({"entity":true})").find("entity id (an integer) or an entity name") != std::string::npos);
    c.ok("component_set", R"({"entity":"Crate A","component":"LocalTransform","path":"position.x","value":-3})");
    CHECK(compact(c.ok("scene_get_entity", R"({"entity":3})").v().get("components").get("LocalTransform")) == R"({"position":[-3,2,0]})");
    c.ok("scene_undo");

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
    // An entity name works where "$<id>" does (unique names only).
    c.ok("component_set", R"({"entity":7,"component":"McpProbe","path":"target","value":null})");
    const std::string name3 = str(c.ok("scene_get_entity", R"({"entity":3})").v().get("name"));
    c.ok("component_set", R"({"entity":7,"component":"McpProbe","value":{"target":")" + name3 + R"("}})");
    CHECK(c.server.session().scene().get<mcptest::McpProbe>(7)->target == editor::doc_entity_ref(3));
    CHECK(c.fails("component_set", R"({"entity":7,"component":"McpProbe","path":"target","value":"Nobody"})").find("no entity with that name") != std::string::npos);
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
    CHECK(c.fails("scene_get_entity", R"({"entity":"Crate A"})").find("more than one entity") != std::string::npos);  // two now
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

    // Changed on disk behind the server's back: merged entity by entity (an added newline changes no
    // entity), and a change to a component we changed too is refused until forced or resolved.
    const std::string file = root + "/assets/scenes/demo.mcscene";
    write_file(file, read_file(file) + "\n");
    c.ok("scene_save");
    CHECK(read_file(file).find("\"Marker\"") != std::string::npos);
    c.ok("component_set", R"({"entity":"Ball","component":"Collider2D","path":"restitution","value":0.37})");
    std::string theirs = read_file(file);
    const usize at = theirs.find_first_of("0123456789", theirs.find("restitution"));
    theirs.replace(at, theirs.find_first_not_of("0123456789.", at) - at, "0.12");
    write_file(file, theirs);
    CHECK(c.fails("scene_save").find("conflict") != std::string::npos);
    CHECK(read_file(file) == theirs);  // nothing written
    c.ok("scene_save", R"({"force":true})");
    CHECK(read_file(file).find("0.37") != std::string::npos);
    c.ok("component_set", R"({"entity":7,"component":"LocalTransform","path":"position.x","value":2})");
    c.ok("scene_save");  // our own last write is not a conflict

    // Ids are saved with the file: after a delete and a reopen the survivors keep theirs.
    c.ok("entity_delete", R"({"entity":2})");
    c.ok("scene_save");
    CHECK(read_file(file).find("\"id\": 7,") != std::string::npos);
    c.ok("scene_open", R"({"path":"scenes/demo.mcscene"})");
    CHECK(str(c.ok("scene_get_entity", R"({"entity":7})").v().get("name")) == "Marker");
    CHECK(c.fails("scene_get_entity", R"({"entity":2})").find("no entity with id 2") != std::string::npos);
    CHECK(c.ok("entity_create", R"({"name":"Later"})").v().get("id").as_number() == 8);
    c.ok("scene_undo");

    // Save As: a new file (new GUID), never over another existing file unless forced; forced, the
    // target keeps its own GUID. 'name' renames the scene.
    auto as = c.ok("scene_save", R"({"path":"scenes/copy.mcscene","name":"copy"})");
    CHECK(str(as.v().get("path")) == "scenes/copy.mcscene");
    CHECK(str(as.v().get("name")) == "copy");
    const std::string copy_guid = str(as.v().get("guid"));
    CHECK(copy_guid != kDemoGuid);
    CHECK(std::filesystem::exists(root + "/assets/scenes/copy.mcscene"));
    CHECK(c.fails("scene_save", R"({"path":"scenes/demo.mcscene"})").find("already exists") != std::string::npos);
    CHECK(c.ok("asset_list", R"({"type":"Scene"})").v().get("assets").size() == 2);
    auto over = c.ok("scene_save", R"({"path":"scenes/demo.mcscene","force":true})");
    CHECK(str(over.v().get("guid")) == kDemoGuid);  // demo.mcscene is still the asset the .mceproj names
    CHECK(read_file(file).find(kDemoGuid) != std::string::npos);
    CHECK(c.ok("asset_list", R"({"type":"Scene"})").v().get("assets").size() == 2);

    // run_headless plays another saved scene by path; asset_delete removes a file and its sidecar.
    auto other = c.ok("run_headless", R"({"scene":"scenes/copy.mcscene","ticks":5,"log_lines":0,"inspect":{"component":"RigidBody2D"}})");
    CHECK(str(other.v().get("scene")) == "scenes/copy.mcscene");
    CHECK(other.v().get("inspect").get("entities").at(0).get("id").is_number());  // the played scene's ids
    // Names and ids in inspect name the played scene's entities.
    auto ball = c.ok("run_headless", R"({"scene":"scenes/copy.mcscene","ticks":5,"log_lines":0,"inspect":{"entities":["Ball"]}})");
    CHECK(ball.v().get("inspect").get("entities").at(0).get("id").as_number() == 5);
    CHECK(c.fails("run_headless", R"({"scene":"scenes/copy.mcscene","inspect":{"entities":[99]}})").find("played scene") != std::string::npos);
    CHECK(c.fails("run_headless", R"({"scene":"scenes/nope.mcscene"})").find("no such scene") != std::string::npos);
    CHECK(c.fails("asset_delete", R"({"path":"scenes/demo.mcscene"})").find("open scene") != std::string::npos);
    CHECK(c.fails("asset_delete", R"({"path":"../x.png"})").find("under assets/") != std::string::npos);
    CHECK(c.fails("asset_delete", R"({"path":"scenes"})").find("directory") != std::string::npos);
    auto del = c.ok("asset_delete", R"({"path":"scenes/copy.mcscene"})");
    CHECK_FALSE(del.v().get("sidecar").as_bool());  // a scene carries its GUID inside: no sidecar
    write_file(root + "/assets/textures/extra.png", read_file(root + "/assets/textures/ball.png"));
    c.ok("cook_run", R"({"adopt":true})");
    CHECK(std::filesystem::exists(root + "/assets/textures/extra.png.mceasset"));
    CHECK(c.ok("asset_delete", R"({"path":"textures/extra.png"})").v().get("sidecar").as_bool());
    CHECK_FALSE(std::filesystem::exists(root + "/assets/textures/extra.png.mceasset"));
    CHECK_FALSE(std::filesystem::exists(root + "/assets/scenes/copy.mcscene"));
    CHECK_FALSE(std::filesystem::exists(root + "/assets/scenes/copy.mcscene.mceasset"));
    CHECK(c.ok("asset_list", R"({"type":"Scene"})").v().get("assets").size() == 1);

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
    auto packed = c.ok("cook_run", R"({"pack":true})");
    CHECK(packed.v().get("ok").as_bool());
    CHECK(std::filesystem::exists(root + "/assets.mce"));
    CHECK(str(packed.v().get("pack")) == "assets.mce");
    CHECK(str(packed.v().get("cooked_dir")) == "cooked");
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

MCE_CVAR(cv_mcp_probe, "test.mcp_probe", 0, "read by probe_game_install's system", CVarFlags::None);
i32 g_probe_seen = -1;
void read_probe(Frame&) { g_probe_seen = cv_mcp_probe.get_int(); }
ResultV probe_game_install(World& w) {
    w.add_system(Phase::FixedUpdate_Pre, "mcp_test_probe", &read_probe);
    return ok();
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

    // convars: set for the run, put back after it (SBA-61: a convar-driven setting can be previewed).
    editor::set_play_game_install(&probe_game_install);
    c.ok("run_headless", R"({"ticks":2,"log_lines":0,"convars":{"test.mcp_probe":5}})");
    CHECK(g_probe_seen == 5);
    CHECK(cv_mcp_probe.get_int() == 0);
    c.ok("run_headless", R"({"ticks":2,"log_lines":0})");
    CHECK(g_probe_seen == 0);
    editor::set_play_game_install(nullptr);
    CHECK(c.fails("run_headless", R"({"convars":{"no.such.cvar":1}})").find("console_list") != std::string::npos);
    CHECK(c.fails("run_headless", R"({"convars":{"test.mcp_probe":"x"}})").find("does not take") != std::string::npos);
    CHECK(cv_mcp_probe.get_int() == 0);
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

TEST_CASE("mcp tools: asset_create / asset_write / asset_read author every JSON source type (M-DATA-REACH 1)") {
    const std::string root = demo_project("mcp_assets");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();

    // Every type asset_schema lists is created from its template and cooks clean.
    auto listed = c.ok("asset_schema");
    const cook::JsonValue types = listed.v().get("types");
    REQUIRE(types.size() == 14);
    for (u32 i = 0; i < types.size(); ++i) {
        const std::string type = str(types.at(i).get("type")), ext = str(types.at(i).get("extension"));
        auto schema = c.ok("asset_schema", R"({"type":")" + type + R"("})");
        CHECK(schema.v().get("fields").is_array());
        CHECK(schema.v().get("template").is_object());
        std::string content = "{}";
        if (ext == ".mctilemap") content = std::string(R"({"tileset":")") + kBallGuid + R"(","columns":2,"rows":2,"width":8,"height":4})";
        auto made = c.ok("asset_create", cat({R"({"path":"made/a)", ext, R"(","content":)", content, "}"}));
        CHECK(str(made.v().get("type")) == type);
        CHECK(str(made.v().get("guid")).rfind("guid:", 0) == 0);
        CHECK(std::filesystem::exists(cat({root, "/assets/made/a", ext})));
        CHECK_FALSE(std::filesystem::exists(cat({root, "/assets/made/a", ext, ".mceasset"})));  // native: GUID inline
        CHECK(compact(c.ok("asset_list", R"({"type":")" + type + R"("})").v()).find("made/a" + ext) != std::string::npos);
    }
    auto cook = c.ok("cook_run");
    CHECK_MESSAGE(cook.v().get("ok").as_bool(), compact(cook.v().get("diagnostics")));

    // Refusals change nothing: an existing file, a bad value, a missing reference, an unknown key,
    // reserved keys, a type asset_create does not make.
    CHECK(c.fails("asset_create", R"({"path":"made/a.mcpart"})").find("already exists") != std::string::npos);
    CHECK(c.fails("asset_create", R"({"path":"made/b.mcpart","content":{"lifetime":[2,1]}})").find("lifetime") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(root + "/assets/made/b.mcpart"));
    CHECK(c.fails("asset_create", R"({"path":"made/b.mcpart","content":{"texture":"guid:5ce4e000-0000-4000-8000-0000000000ff"}})")
              .find("missing reference") != std::string::npos);
    CHECK(c.fails("asset_create", R"({"path":"made/b.mcpart","content":{"colour":[1,1,1,1]}})").find("colour") != std::string::npos);
    CHECK(c.fails("asset_create", R"({"path":"made/b.mcpart","content":{"guid":"x"}})").find("set by the tool") != std::string::npos);
    CHECK(c.fails("asset_create", R"({"path":"made/b.mcscene"})").find("scene_new") != std::string::npos);
    CHECK(c.fails("asset_create", R"({"path":"../b.mcpart"})").find("under assets/") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(root + "/assets/made/b.mcpart"));

    // asset_write merges by default and keeps the GUID; replace drops the rest; a refused write
    // leaves the file as it was.
    const std::string guid = str(c.ok("asset_read", R"({"path":"made/a.mcpart"})").v().get("guid"));
    auto w = c.ok("asset_write", std::string(R"({"path":"made/a.mcpart","content":{"rate":55,"texture":")") + kBallGuid + R"("}})");
    CHECK(str(w.v().get("guid")) == guid);
    auto r = c.ok("asset_read", R"({"guid":")" + guid + R"("})");
    CHECK(r.v().get("content").get("rate").as_number() == 55);
    CHECK(str(r.v().get("content").get("blend")) == "alpha");  // merged: the template's other fields stay
    CHECK(str(r.v().get("type")) == "ParticlePreset");
    const std::string before = read_file(root + "/assets/made/a.mcpart");
    CHECK(c.fails("asset_write", R"({"path":"made/a.mcpart","content":{"max_particles":0}})").find("max_particles") != std::string::npos);
    CHECK(read_file(root + "/assets/made/a.mcpart") == before);
    c.ok("asset_write", R"({"path":"made/a.mcpart","mode":"replace","content":{"name":"bare"}})");
    r = c.ok("asset_read", R"({"path":"made/a.mcpart"})");
    CHECK_FALSE(r.v().get("content").get("rate").valid());
    CHECK(str(r.v().get("content").get("guid")) == guid.substr(5));
    CHECK(c.fails("asset_write", R"({"path":"textures/ball.png","content":{}})").find("asset_set_import_settings") != std::string::npos);
    CHECK(c.fails("asset_read", R"({"path":"textures/ball.png"})").find("not a JSON source asset") != std::string::npos);
    CHECK(str(c.ok("asset_read", R"({"path":"scenes/demo.mcscene"})").v().get("content").get("format")) == "mce-scene");

    // The written asset plays: a ParticleEmitter in the scene names it, and run_headless cooks it.
    c.ok("entity_create", R"({"name":"fx","components":{"ParticleEmitter":{"preset":")" + guid + R"("}}})");
    c.ok("scene_save");
    // It also runs the store catalog, remote config and game services the pack now has (the fake
    // store, the cooked defaults, services on this device; M-DATA-REACH 8).
    CHECK(c.ok("run_headless", R"({"ticks":10})").v().get("ticks").as_number() == 10);
}

TEST_CASE("mcp tools: tilemap_paint paints cells, rects and flood fills in one checked call") {
    const std::string root = demo_project("mcp_tilemap_paint");
    Client c;
    REQUIRE(c.server.open_project(sv(root)));
    c.init();
    c.ok("asset_create", std::string(R"({"path":"maps/level.mctilemap","content":{"tileset":")") + kBallGuid +
                             R"(","columns":2,"rows":2,"width":6,"height":3}})");
    auto p = c.ok("tilemap_paint", R"({"path":"maps/level.mctilemap","rects":[[0,0,5,0,1]],"cells":[[2,2,3],[9,9,1]],"preview":true})");
    CHECK(p.v().get("changed").as_number() == 7);
    CHECK(p.v().get("outside").as_number() == 1);
    CHECK(compact(p.v().get("preview")) == R"(["..3...","......","111111"])");
    // Flood fill: the empty region above the floor, not the floor or the 3 inside it.
    p = c.ok("tilemap_paint", R"({"path":"maps/level.mctilemap","fills":[[0,1,2]],"preview":true})");
    CHECK(p.v().get("changed").as_number() == 11);
    CHECK(compact(p.v().get("preview")) == R"(["223222","222222","111111"])");
    // A tile past the tileset (4 cells) refuses the whole call.
    const std::string before = read_file(root + "/assets/maps/level.mctilemap");
    CHECK(c.fails("tilemap_paint", R"({"path":"maps/level.mctilemap","cells":[[0,0,0]],"rects":[[0,0,1,1,5]]})").find("outside the tileset") != std::string::npos);
    CHECK(read_file(root + "/assets/maps/level.mctilemap") == before);
    CHECK(c.fails("tilemap_paint", R"({"path":"maps/level.mctilemap","cells":[[0,0]]})").find("[x, y, tile]") != std::string::npos);
    CHECK(c.fails("tilemap_paint", R"({"path":"textures/ball.png","clear":0})").find("not a tilemap") != std::string::npos);
    p = c.ok("tilemap_paint", R"({"path":"maps/level.mctilemap","clear":0,"preview":true})");
    CHECK(compact(p.v().get("preview")) == R"(["......","......","......"])");
    CHECK(c.ok("cook_run").v().get("ok").as_bool());
}

TEST_CASE("mcp tools: prefabs over MCP: create from an entity, place instances, apply, revert, edit (M-DATA-REACH 2)") {
    const std::string root = demo_project("mcp_prefabs");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    const auto count = [&] { return c.ok("scene_info").v().get("entities").as_number(); };
    // A child under Crate A goes into the prefab with it.
    c.ok("entity_create", std::string(R"({"name":"Lid","components":{"ChildOf":{"parent":"$3"},"Sprite":{"texture":")") + kBallGuid + R"("}}})");
    const double before = count();

    auto made = c.ok("prefab_create_from", R"({"entity":"Crate A","path":"prefabs/crate.mcprefab"})");
    const std::string guid = str(made.v().get("guid"));
    const double inst = made.v().get("instance").as_number();
    CHECK(count() == before - 1);  // Crate A + Lid -> one instance
    auto prefab_file = parse(read_file(root + "/assets/prefabs/crate.mcprefab"));
    CHECK(str(prefab_file->root().get("format")) == "mce-prefab");
    REQUIRE(prefab_file->root().get("entities").size() == 2);
    CHECK(str(prefab_file->root().get("entities").at(0).get("name")) == "Crate A");
    auto placed = c.ok("scene_get_entity", R"({"entity":"Crate A"})");
    CHECK(placed.v().get("id").as_number() == inst);
    CHECK(str(placed.v().get("prefab")) == guid);
    CHECK(compact(placed.v().get("components").get("LocalTransform")).find("[-2,2,0]") != std::string::npos);
    CHECK(c.fails("prefab_create_from", R"({"entity":"Crate B","path":"prefabs/crate.mcprefab"})").find("crate.mcprefab") != std::string::npos);
    CHECK(c.fails("prefab_create_from", R"({"entity":"Crate B","path":"prefabs/crate.txt"})").find(".mcprefab") != std::string::npos);

    // One undo step brings the entities back; redo makes the instance again.
    CHECK(str(c.ok("scene_undo").v().get("undone")) == "prefab_create_from");
    CHECK(count() == before);
    c.ok("scene_redo");
    CHECK(count() == before - 1);

    // Four more instances placed by MCP; one overrides the root's Sprite.
    for (int i = 0; i < 4; ++i) {
        const std::string x = std::to_string(3 + i), n = std::to_string(i);
        const std::string_view sprite = i == 0 ? R"(,"Sprite":{"size":[2,2]})" : "";
        auto r = c.ok("entity_create", cat({R"({"name":"Crate )", n, R"(","prefab":")", guid,
                                            R"(","components":{"LocalTransform":{"position":[)", x, R"(,2,0]})", sprite, "}}"}));
        CHECK(str(r.v().get("prefab")) == guid);
    }
    CHECK(c.fails("entity_create", std::string(R"({"name":"x","prefab":")") + kBallGuid + R"("})").find(".mcprefab") != std::string::npos);
    c.ok("scene_save");
    // Five crates of two entities each, Crate B, the ball and the ground: 13 Sprites.
    auto run = c.ok("run_headless", R"({"ticks":5,"inspect":{"component":"Sprite"}})");
    CHECK(run.v().get("inspect").get("count").as_number() == 13);

    // prefab_apply writes the override into the prefab; prefab_revert drops one.
    auto applied = c.ok("prefab_apply", R"({"entity":"Crate 0"})");
    CHECK(compact(applied.v().get("applied")) == R"(["Sprite"])");
    prefab_file = parse(read_file(root + "/assets/prefabs/crate.mcprefab"));
    CHECK(compact(prefab_file->root().get("entities").at(0).get("components").get("Sprite")).find("[2,2]") != std::string::npos);
    CHECK(c.ok("scene_get_entity", R"({"entity":"Crate 0"})").v().get("components").get("Sprite").valid() == false);
    c.ok("component_set", R"({"entity":"Crate 1","component":"Sprite","path":"size","value":[3,3]})");
    CHECK(compact(c.ok("prefab_revert", R"({"entity":"Crate 1"})").v().get("reverted")) == R"(["Sprite"])");
    CHECK(c.fails("prefab_apply", R"({"entity":"Crate B"})").find("not a prefab instance") != std::string::npos);
    CHECK(c.fails("prefab_apply", R"({"entity":"Crate 1","component":"LocalTransform"})").find("placement") != std::string::npos);
    c.ok("scene_save");

    // The prefab itself opens and saves like a scene, and stays a prefab.
    c.ok("scene_open", R"({"path":"prefabs/crate.mcprefab"})");
    CHECK(count() == 2);
    c.ok("component_set", R"({"entity":"Lid","component":"Sprite","path":"size","value":[0.5,0.5]})");
    c.ok("scene_save");
    prefab_file = parse(read_file(root + "/assets/prefabs/crate.mcprefab"));
    CHECK(str(prefab_file->root().get("format")) == "mce-prefab");
    CHECK(c.ok("cook_run").v().get("ok").as_bool());
}

TEST_CASE("mcp tools: project_get / project_set and the scene's WorldSettings (M-DATA-REACH 3)") {
    const std::string root = demo_project("mcp_project_settings");
    write_file(root + "/Demo.mceproj", std::string(R"({
  "format": "mce-project",
  "version": 1,
  "name": "Demo",
  "title": "Demo",
  "games": [],
  "start_scene": ")") + kDemoGuid + R"(",
  "plugins": []
}
)");
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    auto p = c.ok("project_get");
    CHECK(str(p.v().get("file")) == "Demo.mceproj");
    CHECK(str(p.v().get("start_scene_path")) == "scenes/demo.mcscene");
    CHECK(p.v().get("world_settings").is_null());

    // A copy of the scene becomes the start scene, by path; the title changes; the rest stays.
    c.ok("scene_save", R"({"path":"scenes/other.mcscene"})");
    p = c.ok("project_set", R"({"values":{"title":"Other Demo","start_scene":"scenes/other.mcscene"}})");
    CHECK(str(p.v().get("start_scene_path")) == "scenes/other.mcscene");
    CHECK(str(p.v().get("project").get("title")) == "Other Demo");
    auto file = parse(read_file(root + "/Demo.mceproj"));
    CHECK(str(file->root().get("format")) == "mce-project");
    CHECK(str(file->root().get("start_scene")) == str(c.ok("scene_info").v().get("guid")));
    CHECK(file->root().get("plugins").is_array());
    const std::string before = read_file(root + "/Demo.mceproj");
    CHECK(c.fails("project_set", R"({"values":{"start_scene":"textures/ball.png"}})").find("start_scene") != std::string::npos);
    CHECK(c.fails("project_set", R"({"values":{"gravity":[0,1]}})").find("not a project setting") != std::string::npos);
    CHECK(c.fails("project_set", R"({"values":{"title":"x","games":[1]}})").find("games") != std::string::npos);
    CHECK(read_file(root + "/Demo.mceproj") == before);
    // A patch without start_scene keeps it.
    p = c.ok("project_set", R"({"values":{"title":"Third Demo"}})");
    CHECK(str(p.v().get("start_scene_path")) == "scenes/other.mcscene");
    CHECK(str(parse(read_file(root + "/Demo.mceproj"))->root().get("start_scene")) == str(c.ok("scene_info").v().get("guid")));

    // WorldSettings: a bad value creates nothing; zero 2D gravity keeps the ball where it starts.
    CHECK(c.fails("scene_settings_set", R"({"values":{"gravity_2d":"down"}})").find("gravity_2d") != std::string::npos);
    CHECK(c.ok("scene_settings_get").v().get("world_settings").is_null());
    auto ws = c.ok("scene_settings_set", R"({"values":{"gravity_2d":[0,0]}})");
    CHECK(compact(ws.v().get("world_settings").get("value")).find("[0,0]") != std::string::npos);
    CHECK(str(c.ok("scene_get_entity", R"({"entity":"World Settings"})").v().get("name")) == "World Settings");
    c.ok("scene_save");
    auto run = c.ok("run_headless", R"({"ticks":60,"inspect":{"entities":["Ball"],"show":["LocalTransform"]}})");
    const cook::JsonValue pos = run.v().get("inspect").get("entities").at(0).get("components").get("LocalTransform").get("position");
    CHECK(pos.at(1).as_number() == doctest::Approx(6.0));
    c.ok("scene_settings_set", R"({"values":{"gravity_2d":[0,-9.81]}})");
    c.ok("scene_save");
    run = c.ok("run_headless", R"({"ticks":60,"inspect":{"entities":["Ball"],"show":["LocalTransform"]}})");
    CHECK(run.v().get("inspect").get("entities").at(0).get("components").get("LocalTransform").get("position").at(1).as_number() < 5.0);
}

// The M-DATA-REACH demo (SBA-47): a 2D and a 3D game authored entirely through MCP tools, with no
// game C++: every behaviour below is a stock component the scene names. run_headless plays each
// twice; the state hashes match (the replay gate).
TEST_CASE("M-DATA-REACH demo: a 2D game made over MCP alone (sheet animation, follow + shake camera, pickup sound and burst, five prefab enemies, a painted tilemap)") {
    const std::string root = demo_project("mcp_demo2d");
    write_file(root + "/assets/audio/pickup.wav", make_wav(22050, 1, 2205, 880.0));
    Client c;
    REQUIRE(c.server.open_project(sv(root), "scenes/demo.mcscene"));
    c.init();
    // The sound has no sidecar yet: cook_run adopt registers it, and asset_list names its GUID.
    REQUIRE(c.ok("cook_run", R"({"adopt":true})").v().get("ok").as_bool());
    auto listed = c.ok("asset_list", R"({"type":"Sfx"})");
    const cook::JsonValue sfx_list = listed.v().get("assets");
    REQUIRE(sfx_list.size() == 1);
    const std::string sfx = str(sfx_list.at(0).get("guid"));
    constexpr const char* kCrateGuid = "guid:5ce4e000-0000-4000-8000-000000000001";

    // Assets: a 2x2 sprite sheet over the crate texture, a particle preset, a tilemap the agent paints.
    const std::string sheet = str(c.ok("asset_create", std::string(R"({"path":"sprites/hero.mcsheet","content":{"texture":")") + kCrateGuid +
                                                           R"(","grid":{"columns":2,"rows":2},"clips":[{"name":"run","frames":{"from":0,"to":3},"fps":10}]}})")
                                      .v().get("guid"));
    const std::string sparks = str(c.ok("asset_create", R"({"path":"effects/sparks.mcpart","content":{"rate":0,"lifetime":[0.3,0.6]}})").v().get("guid"));
    const std::string level = str(c.ok("asset_create", std::string(R"({"path":"maps/level.mctilemap","content":{"tileset":")") + kCrateGuid +
                                                           R"(","columns":1,"rows":1,"width":24,"height":3}})")
                                      .v().get("guid"));
    auto painted = c.ok("tilemap_paint", R"({"path":"maps/level.mctilemap","rects":[[0,0,23,0,1],[4,1,6,1,1]],"cells":[[12,2,1]],"preview":true})");
    CHECK(painted.v().get("changed").as_number() == 28);

    const auto make = [&](const std::string& args) { return static_cast<int>(c.ok("entity_create", args).v().get("id").as_number()); };
    const int hero = make(cat({R"({"name":"Hero","components":{"LocalTransform":{"position":[-6,3,0]},"RigidBody2D":{"type":"Dynamic"},)",
                               R"("Collider2D":{"shape":{"type":"Circle","radius":0.4}},"Sprite":{"texture":")", kCrateGuid, R"("},)",
                               R"("AnimatedSprite":{"sheet":")", sheet, R"(","clip":"run"}}})"}));
    make(cat({R"({"name":"Coin","components":{"LocalTransform":{"position":[-6,0,0]},"RigidBody2D":{},)",
              R"("Collider2D":{"shape":{"half":[1,0.5]},"is_sensor":true},"PhysicsEvents":{},)",
              R"("SfxTrigger":{"clip":")", sfx, R"(","on":"TriggerEnter"},)",
              R"("ParticleEmitter":{"preset":")", sparks, R"(","playing":false},"ParticleBurst":{"count":30,"on":"TriggerEnter"},)",
              R"("ShakeTrigger":{"trauma":0.8,"on":"TriggerEnter"}}})"}));
    c.ok("component_add", cat({R"({"entity":"Camera","component":"CameraFollow2D","value":{"target":"$)", std::to_string(hero), R"(","smoothing":0}})"}));
    c.ok("component_add", R"({"entity":"Camera","component":"CameraShake2D","value":{"decay_per_second":0.2}})");
    make(cat({R"({"name":"Level","components":{"LocalTransform":{"position":[-12,-6,0]},"TilemapLayer":{"map":")", level, R"("}}})"}));

    // One enemy, made a prefab, placed four more times.
    make(cat({R"({"name":"Enemy","components":{"LocalTransform":{"position":[4,0,0]},"RigidBody2D":{"type":"Dynamic"},)",
              R"("Collider2D":{"shape":{"type":"Circle","radius":0.4}},"Sprite":{"texture":")", kBallGuid, R"("},)",
              R"("AnimatedSprite":{"sheet":")", sheet, R"("}}})"}));
    const std::string enemy = str(c.ok("prefab_create_from", R"({"entity":"Enemy","path":"prefabs/enemy.mcprefab"})").v().get("guid"));
    for (int i = 1; i < 5; ++i)
        make(cat({R"({"name":"Enemy )", std::to_string(i), R"(","prefab":")", enemy, R"(","components":{"LocalTransform":{"position":[)",
                  std::to_string(4 + i), R"(,)", std::to_string(i), R"(,0]}}})"}));
    c.ok("scene_save");
    REQUIRE(c.ok("cook_run").v().get("ok").as_bool());

    // Six animated sprites (the hero, five enemies), every one stepping through its clip.
    const std::string args = R"({"ticks":120,"log_lines":0,"inspect":{"component":"AnimatedSprite","show":["AnimatedSprite"]}})";
    auto run = c.ok("run_headless", args);
    const cook::JsonValue animated = run.v().get("inspect").get("entities");
    REQUIRE(animated.size() == 6);
    for (u32 i = 0; i < animated.size(); ++i) {
        CHECK(str(animated.at(i).get("components").get("AnimatedSprite").get("playing_clip")) == "run");
        CHECK(animated.at(i).get("components").get("AnimatedSprite").get("time").as_number() > 1.5);
    }
    CHECK(str(animated.at(5).get("name")) == "Enemy 4");
    // The hero fell through the coin: one sound, one burst, one shake; the camera sits on the hero.
    auto world = c.ok("run_headless", R"({"ticks":120,"log_lines":0,"inspect":{"entities":["Hero","Coin","Camera"]}})");
    CHECK(str(world.v().get("state_hash")) == str(run.v().get("state_hash")));  // replays match
    const cook::JsonValue ents = world.v().get("inspect").get("entities");
    REQUIRE(ents.size() == 3);  // in entity order
    const cook::JsonValue cam = ents.at(0).get("components"), hero_c = ents.at(1).get("components"), coin = ents.at(2).get("components");
    CHECK(str(ents.at(2).get("name")) == "Coin");
    CHECK(coin.get("SfxTrigger").get("fired").as_number() == 1);
    CHECK(coin.get("ParticleBurst").get("fired").as_number() == 1);
    CHECK(coin.get("ShakeTrigger").get("fired").as_number() == 1);
    CHECK(str(coin.get("PhysicsEvents").get("last_trigger")) == "$" + std::to_string(hero));
    CHECK(hero_c.get("LocalTransform").get("position").at(1).as_number() < -1.0);  // landed on the ground
    CHECK(compact(cam.get("LocalTransform").get("position").at(0)) == compact(hero_c.get("LocalTransform").get("position").at(0)));
    CHECK(cam.get("CameraShake2D").get("trauma").as_number() > 0.5);
    CHECK(world.v().get("log").get("errors").as_number() == 0);
}

TEST_CASE("M-DATA-REACH demo: a 3D game made over MCP alone (a character controller dropped onto a dynamic crate pile, an orbit camera)") {
    const std::string root = demo_project("mcp_demo3d");
    Client c;
    REQUIRE(c.server.open_project(sv(root)));
    c.init();
    c.ok("scene_new", R"({"name":"crates"})");
    const auto make = [&](const std::string& args) { return static_cast<int>(c.ok("entity_create", args).v().get("id").as_number()); };
    make(R"({"name":"Floor","components":{"LocalTransform":{"position":[0,-0.5,0]},"BoxCollider3D":{"half":[20,0.5,20]}}})");
    // A pyramid of six 1 m crates: three, two, one.
    const char* pile[] = {"[0,0.5,-1.05]", "[0,0.5,0]", "[0,0.5,1.05]", "[0,1.5,-0.525]", "[0,1.5,0.525]", "[0,2.5,0]"};
    for (int i = 0; i < 6; ++i)
        make(cat({R"({"name":"Crate )", std::to_string(i), R"(","components":{"LocalTransform":{"position":)", pile[i],
                  R"(},"RigidBody3D":{"half":[0.5,0.5,0.5]}}})"}));
    const int hero = make(R"({"name":"Hero","components":{"LocalTransform":{"position":[0,6,0]},"CharacterController3D":{}}})");
    make(cat({R"({"name":"Camera","components":{"LocalTransform":{},"Camera3DComponent":{},"CameraFollow3D":{"target":"$)",
              std::to_string(hero), R"(","distance":8}}})"}));
    c.ok("scene_save", R"({"path":"scenes/crates.mcscene"})");
    const std::string args = R"({"ticks":240,"log_lines":0,"inspect":{"component":"LocalTransform","show":["LocalTransform","CharacterController3D","RigidBody3D"]}})";
    auto run = c.ok("run_headless", args);
    CHECK(str(c.ok("run_headless", args).v().get("state_hash")) == str(run.v().get("state_hash")));  // replays match
    const cook::JsonValue ents = run.v().get("inspect").get("entities");
    REQUIRE(ents.size() == 9);  // in entity order: floor, six crates, hero, camera
    // The pile holds: every crate rests where it was stacked.
    const double rest_y[] = {0.5, 0.5, 0.5, 1.5, 1.5, 2.5};
    for (u32 i = 0; i < 6; ++i) {
        const cook::JsonValue body = ents.at(i + 1).get("components");
        CHECK(str(body.get("RigidBody3D").get("type")) == "Dynamic");
        CHECK(body.get("LocalTransform").get("position").at(1).as_number() == doctest::Approx(rest_y[i]).epsilon(0.01));
    }
    // The hero fell onto the top crate and stands on it (capsule centre 0.9 m above its top at 3 m).
    const cook::JsonValue hero_c = ents.at(7).get("components");
    CHECK(hero_c.get("CharacterController3D").get("grounded").as_bool());
    CHECK(hero_c.get("LocalTransform").get("position").at(1).as_number() == doctest::Approx(3.9).epsilon(0.01));
    // The camera follows it down: behind it and above it.
    const cook::JsonValue cam = ents.at(8).get("components").get("LocalTransform").get("position");
    CHECK(cam.at(2).as_number() > 5.0);
    CHECK(cam.at(1).as_number() > 3.9);
    CHECK(run.v().get("log").get("errors").as_number() == 0);
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
        c.ok("component_set", R"({"entity":6,"component":"TextLabel","value":{"text":"EDITED BY MCP","outline_width":0.125,"shadow_offset":[0.0625,-0.0625]}})");
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
    tl.outline_width = 0.125f;  // the text's look (M-UI-DATA 4) saves like any field
    tl.shadow_offset = {0.0625f, -0.0625f};
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

#if defined(MCE_PROBE_MODULE)
// The stock server with a game module loaded (mce_mcp --module): the module's reflected components
// register as it maps, so the project's scenes (which use them) cook and list; its install() runs in
// run_headless, so the run hashes exactly like the project's own host playing the same scene.
TEST_CASE("mcp server: a loaded game module cooks its components and plays like the project's host") {
    const std::string root = fresh_dir("mcp_probe");
    std::filesystem::copy(std::string(MCE_PROBE_PROJECT) + "/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    {
        Client bare;  // without the module the game's components are unknown: the project's cook fails
        REQUIRE(bare.server.open_project(sv(root)));
        CHECK_FALSE(bare.server.session().cook().ok());
    }

    Client c;
    REQUIRE(c.server.load_module(MCE_PROBE_MODULE));
    CHECK(std::string(c.server.module().info->name) == "template_probe");
    CHECK_FALSE(c.server.load_module(MCE_PROBE_MODULE));  // one module per server
    REQUIRE(c.server.open_project(sv(root), "scenes/main.mcscene"));
    c.init();
    auto comps = c.ok("refl_list_components");
    bool spin = false, bob = false;
    const cook::JsonValue list = comps.v().get("components");
    for (u32 i = 0; i < list.size(); ++i) {
        spin |= str(list.at(i).get("name")) == "Spin";
        bob |= str(list.at(i).get("name")) == "Bob";
    }
    CHECK(spin);
    CHECK(bob);
    CHECK(c.ok("refl_describe_component", R"({"name":"Spin"})").v().get("fields").size() == 1);

    auto run = c.ok("run_headless", R"({"ticks":300,"log_lines":0,"inspect":{"component":"Bob"}})");
    const std::string hash = str(run.v().get("state_hash"));
    const cook::JsonValue bobs = run.v().get("inspect").get("entities");
    REQUIRE(bobs.size() >= 1);
    CHECK(bobs.at(0).get("components").get("Bob").get("started").as_bool());  // the game's system ran

    const StringView args[] = {MCE_PROBE_HOST, "-headless", "1", "-ticks", "300"};
    platform::Process host;
    REQUIRE(host.start(Span<const StringView>(args, 5), true));
    Array<char> out;
    const i32 code = host.wait(&out);
    const std::string text(out.data(), out.size());
    CHECK_MESSAGE(code == 0, text);
    const bool played = text.find("300 ticks, ") != std::string::npos && text.find("state_hash " + hash) != std::string::npos;
    CHECK_MESSAGE(played, text);
}
#endif

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
