// The Animation panel's editor (ROADMAP M-CINEMA step 3), headless: a .mcanim's canonical text
// round-trips through the cook, state / transition / parameter edits are refused with the cook's
// message and land on disk with undo and redo, a mesh's clips are listed, and the preview poses a
// clip and runs the graph in a paused play session without touching the document.

#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "cook_test_util.h"
#include "mce/cook/animgraph_text.h"
#include "mce/cook/fs.h"
#include "mce/cook/json.h"
#include "mce/ecs/transform.h"
#include "mce/editor/animgraph_editor.h"
#include "mce/editor/editor_session.h"
#include "mce/kit/kit3d.h"
#include "mce/string_id.h"

using namespace mce;
using namespace mce::editor;
using namespace cook_test;

namespace {

StringView sv(const std::string& s) { return StringView(s.data(), s.size()); }
std::string text_of(const cook::String& s) { return std::string(s.view().data(), s.view().size()); }

std::string file_text(const std::string& path) {
    cook::String s;
    REQUIRE(cook::fs::read_text(sv(path), s));
    return text_of(s);
}

std::string serialized(const EditorScene& s) {
    cook::String t;
    s.serialize(t);
    return text_of(t);
}

std::string base64(const std::string& bytes) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    usize i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const u32 v = (u32(u8(bytes[i])) << 16) | (u32(u8(bytes[i + 1])) << 8) | u32(u8(bytes[i + 2]));
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += k[(v >> 6) & 63];
        out += k[v & 63];
    }
    if (i < bytes.size()) {
        u32 v = u32(u8(bytes[i])) << 16;
        if (i + 1 < bytes.size()) v |= u32(u8(bytes[i + 1])) << 8;
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? k[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

void put(std::string& b, std::initializer_list<f32> v) {
    for (f32 f : v) b.append(reinterpret_cast<const char*>(&f), sizeof(f));
}

// A one-triangle glTF with two translation clips: "Idle" (1 s, x 0 -> 1) and "Walk" (2 s, x 0 -> 4).
std::string animated_gltf() {
    std::string b;
    put(b, {0, 0, 0, 1, 0, 0, 0, 1, 0});  // 0: positions, 36 bytes
    put(b, {0, 1});                       // 36: Idle times
    put(b, {0, 0, 0, 1, 0, 0});           // 44: Idle values
    put(b, {0, 2});                       // 68: Walk times
    put(b, {0, 0, 0, 4, 0, 0});           // 76: Walk values
    return R"({ "asset": { "version": "2.0" },
      "buffers": [ { "byteLength": 100, "uri": "data:application/octet-stream;base64,)" + base64(b) + R"(" } ],
      "bufferViews": [ { "buffer": 0, "byteOffset": 0, "byteLength": 36 }, { "buffer": 0, "byteOffset": 36, "byteLength": 8 },
                       { "buffer": 0, "byteOffset": 44, "byteLength": 24 }, { "buffer": 0, "byteOffset": 68, "byteLength": 8 },
                       { "buffer": 0, "byteOffset": 76, "byteLength": 24 } ],
      "accessors": [
        { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0] },
        { "bufferView": 1, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [1] },
        { "bufferView": 2, "componentType": 5126, "count": 2, "type": "VEC3" },
        { "bufferView": 3, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [2] },
        { "bufferView": 4, "componentType": 5126, "count": 2, "type": "VEC3" } ],
      "meshes": [ { "primitives": [ { "attributes": { "POSITION": 0 } } ] } ],
      "nodes": [ { "name": "body", "mesh": 0 } ],
      "scenes": [ { "nodes": [0] } ], "scene": 0,
      "animations": [
        { "name": "Idle", "channels": [ { "sampler": 0, "target": { "node": 0, "path": "translation" } } ],
          "samplers": [ { "input": 1, "output": 2 } ] },
        { "name": "Walk", "channels": [ { "sampler": 0, "target": { "node": 0, "path": "translation" } } ],
          "samplers": [ { "input": 3, "output": 4 } ] } ] })";
}

const char* kGraph = R"({ "format": "mce-animgraph", "version": 1, "guid": "7e400000-0000-4000-8000-000000000002",
    "name": "walker", "params": ["speed"],
    "states": [
      { "name": "move", "blend": { "param": "speed", "points": [ { "clip": "Idle", "at": 0 }, { "clip": "Walk", "at": 1 } ] },
        "events": [ { "time": 0.5, "name": "step" } ] },
      { "name": "wave", "clip": "Walk", "loop": false, "speed": 1.5 } ],
    "transitions": [
      { "from": "move", "to": "wave", "when": [ { "param": "speed", "op": ">=", "value": 2 } ], "fade": 0.1 },
      { "from": "wave", "to": "move", "on_finish": true } ] })";

std::string anim_project(const char* name) {
    const std::string r = fresh_dir(name);
    write_file(r + "/assets/walker.gltf", animated_gltf());
    write_file(r + "/assets/walker.gltf.mceasset", R"({ "guid": "7e400000-0000-4000-8000-000000000001", "importer": "gltf" })");
    write_file(r + "/assets/walker.mcanim", kGraph);
    write_file(r + "/assets/stage.mcscene", R"({ "format": "mce-scene", "version": 1,
        "guid": "7e400000-0000-4000-8000-000000000003", "name": "stage", "entities": [
        { "name": "Walker", "components": { "LocalTransform": {},
            "MeshRef": { "mesh": "guid:7e400000-0000-4000-8000-000000000001" },
            "Animator": { "graph": "guid:7e400000-0000-4000-8000-000000000002" } } } ] })");
    return r;
}

const asset::Guid kMeshGuid = asset::parse_guid("7e400000-0000-4000-8000-000000000001").value();
const asset::Guid kGraphGuid = asset::parse_guid("7e400000-0000-4000-8000-000000000002").value();

void open_stage(EditorSession& session, const std::string& root) {
    REQUIRE(session.open_project(sv(root)));
    REQUIRE(session.open_scene("stage.mcscene"));
}

}  // namespace

TEST_CASE("animgraph editor: the canonical text reads back to the same document and chunk") {
    const std::string root = anim_project("ag_ed_roundtrip");
    EditorSession session;
    open_stage(session, root);
    AnimGraphEditor ed;
    REQUIRE(ed.open(session, kGraphGuid));
    REQUIRE(ed.doc().states.size() == 2);
    CHECK(ed.doc().states[0].is_blend());
    CHECK(ed.doc().states[1].speed == doctest::Approx(1.5f));
    CHECK(ed.doc().transitions.size() == 2);
    CHECK(ed.doc().transitions[0].when[0].op == asset::AnimCompare::GreaterEqual);

    // The cooked file and the editor's chunk agree byte for byte.
    Array<byte> chunk;
    ed.chunk(chunk);
    REQUIRE(asset::AnimGraphView::read(chunk.span()));
    const std::string a = text_of(ed.text());
    cook::JsonDoc doc;
    cook::JsonError err;
    REQUIRE(doc.parse(sv(a), err));
    cook::AnimGraphDocument back;
    cook::DiagList diags;
    REQUIRE(cook::read_animgraph_document(doc.root(), "walker.mcanim", back, diags));
    cook::String b;
    cook::write_animgraph_document(back, kGraphGuid, b);
    CHECK(text_of(b) == a);
    Array<byte> chunk_b;
    cook::build_animgraph_chunk(back, chunk_b);
    REQUIRE(chunk.size() == chunk_b.size());
    CHECK(std::memcmp(chunk.data(), chunk_b.data(), chunk.size()) == 0);

    // States without a position get one on a grid.
    f32 x0, y0, x1, y1;
    ed.position(0, x0, y0);
    ed.position(1, x1, y1);
    CHECK(x1 > x0);

    // The mesh's clips, in file order.
    Array<MeshClipInfo> clips;
    REQUIRE(mesh_clips(session, kMeshGuid, clips));
    REQUIRE(clips.size() == 2);
    CHECK(clips[0].name.view() == StringView("Idle"));
    CHECK(clips[1].duration == doctest::Approx(2.0f));
}

TEST_CASE("animgraph editor: edits, refusals, undo and redo on disk") {
    const std::string root = anim_project("ag_ed_edits");
    EditorSession session;
    open_stage(session, root);
    AnimGraphEditor ed;
    REQUIRE(ed.open(session, kGraphGuid));
    const std::string file = root + "/assets/walker.mcanim";
    const std::string original = file_text(file);

    // A state; names stay unique.
    REQUIRE(ed.add_state("wave", "Idle", 300.0f, 200.0f));
    CHECK(ed.doc().states.size() == 3);
    CHECK(ed.doc().states[2].name.view() == StringView("wave 2"));
    CHECK(ed.selected_state() == 2);
    // A transition from a looping state waits on the first parameter.
    REQUIRE(ed.add_transition(0, 2));
    CHECK(ed.doc().transitions[2].when.size() == 1);
    CHECK(ed.selected_transition() == 2);
    // A one-shot's transition waits for its end.
    REQUIRE(ed.add_transition(1, 2));
    CHECK(ed.doc().transitions[3].on_finish);
    // A name too long for the cooked struct: the cook's message, nothing changes.
    cook::AnimGraphDocState s = ed.doc().states[2];
    s.name.assign("a state name that is far too long for the graph");
    const u64 rev = ed.revision();
    CHECK_FALSE(ed.set_state(2, s));
    CHECK(std::string(ed.last_error().data(), ed.last_error().size()).find("1 to 31 characters") != std::string::npos);
    CHECK(ed.revision() == rev);
    // Renaming keeps the transitions pointing at the state.
    s.name.assign("greet");
    REQUIRE(ed.set_state(2, s));
    CHECK(text_of(ed.text()).find("\"to\": \"greet\"") != std::string::npos);
    // A parameter in use cannot go; a new one can.
    CHECK_FALSE(ed.remove_param(0));
    REQUIRE(ed.add_param("jump"));
    CHECK(ed.doc().params.size() == 2);
    // Removing a state drops its transitions and keeps the others' indices right.
    REQUIRE(ed.remove_state(1));
    CHECK(ed.doc().states.size() == 2);
    CHECK(ed.doc().transitions.size() == 1);
    CHECK(ed.doc().transitions[0].to == 1);
    REQUIRE(ed.set_start(1));
    REQUIRE(ed.move_state(0, 12.4f, 30.6f));
    CHECK(ed.doc().states[0].pos[0] == 12.0f);

    REQUIRE(ed.save(session));
    const std::string saved = file_text(file);
    CHECK(saved == text_of(ed.text()));
    CHECK(saved.find("\"start\": \"greet\"") != std::string::npos);
    CHECK(session.project().last_report().ok());

    REQUIRE(ed.undo(session));
    CHECK(ed.doc().states.size() == 2);
    CHECK(ed.doc().states[1].name.view() == StringView("wave"));
    REQUIRE(ed.redo(session));
    CHECK(file_text(file) == saved);
    (void)original;

    // create(): a fresh graph with one state.
    AnimGraphEditor fresh;
    Result<asset::Guid> g = fresh.create(session, "anim/new.mcanim", "Walk");
    REQUIRE(g);
    CHECK(fresh.doc().states.size() == 1);
    CHECK(fresh.doc().states[0].clip.view() == StringView("Walk"));
    CHECK_FALSE(fresh.add_transition(0, 0));  // no parameter to wait on
    CHECK_FALSE(fresh.create(session, "anim/new.mcanim"));
}

TEST_CASE("animgraph editor: the preview poses clips and runs the graph without touching the document") {
    const std::string root = anim_project("ag_ed_preview");
    EditorSession session;
    open_stage(session, root);
    const std::string before = serialized(session.scene());
    const EntityId walker = session.scene().doc().at(0).id;
    AnimGraphEditor ed;
    REQUIRE(ed.open(session, kGraphGuid));
    Array<EntityId> users;
    ed.users(session.scene(), users);
    REQUIRE(users.size() == 1);
    CHECK(users[0] == walker);

    AnimPreview preview;
    REQUIRE(preview.start(session, walker));
    REQUIRE(preview.running(session));
    World& w = *session.play().world();
    REQUIRE(preview.show_clip(session, "Walk", 1.0f));
    const kit::Animator* a = w.try_get<kit::Animator>(preview.player());
    REQUIRE(a != nullptr);
    CHECK(a->playing == StringId("Walk").value);
    CHECK(a->time == doctest::Approx(1.0f));
    CHECK(a->graph.guid.is_null());

    // Back to the graph: high speed takes it to "wave".
    const f32 params[4] = {3.0f, 0.0f, 0.0f, 0.0f};
    REQUIRE(preview.run_graph(session, params));
    session.play().pause();
    for (int i = 0; i < 10; ++i) session.play().step();
    CHECK(preview.state(session) == StringView("wave"));

    preview.stop(session);
    CHECK_FALSE(session.play().active());
    CHECK(serialized(session.scene()) == before);
    CHECK_FALSE(session.scene().dirty());

    AnimPreview none;
    CHECK_FALSE(none.start(session, kNoEntity));
}
