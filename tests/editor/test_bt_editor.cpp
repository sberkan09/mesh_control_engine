// The Behavior Tree panel's editor (ROADMAP M-AI step 5), headless: a .mcbt's canonical text
// round-trips through the cook's reader, writer and cook; node and key edits are refused with the
// cook's message and land on disk with undo and redo; a tree that does not cook opens with its
// problems and can be fixed; and the debugger reads a play-in-editor brain's node statuses, utility
// scores and blackboard, and runs unsaved edits in the play world.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string>

#include "cook_test_util.h"
#include "mce/cook/bt_text.h"
#include "mce/cook/fs.h"
#include "mce/cook/json.h"
#include "mce/editor/bt_editor.h"
#include "mce/editor/editor_session.h"
#include "mce/kit/ai.h"

using namespace mce;
using namespace mce::editor;
using namespace cook_test;
using kit::BtStatus;

namespace {

StringView sv(const std::string& s) { return StringView(s.data(), s.size()); }
std::string text_of(const cook::String& s) { return std::string(s.view().data(), s.view().size()); }

std::string file_text(const std::string& path) {
    cook::String s;
    REQUIRE(cook::fs::read_text(sv(path), s));
    return text_of(s);
}

const char* kTree = R"({ "format": "mce-bt", "version": 1, "guid": "7e500000-0000-4000-8000-000000000002", "name": "guard",
  "blackboard": { "alert": { "type": "bool", "default": false }, "health": { "type": "float", "default": 50 },
                  "home": "vec3", "target": "entity" },
  "root": { "selector": [
    { "guard": { "sequence": [ { "task": "log", "text": "alert!" }, { "wait": 1 } ] }, "key": "alert", "value": true, "abort": "lower", "name": "alarm" },
    { "utility": [
      { "score": { "wait": 2 }, "base": 0.1, "by": [ { "key": "health", "to": 100 } ] },
      { "score": { "emit": "bored" }, "base": 0.2 } ] } ] } })";

std::string bt_project(const char* name, const char* tree = kTree) {
    const std::string r = fresh_dir(name);
    write_file(r + "/assets/guard.mcbt", tree);
    write_file(r + "/assets/stage.mcscene", R"({ "format": "mce-scene", "version": 1,
        "guid": "7e500000-0000-4000-8000-000000000003", "name": "stage", "entities": [
        { "name": "Guard", "components": { "LocalTransform": {},
            "AiBrain": { "tree": "guid:7e500000-0000-4000-8000-000000000002" } } } ] })");
    return r;
}

const asset::Guid kTreeGuid = asset::parse_guid("7e500000-0000-4000-8000-000000000002").value();

void open_stage(EditorSession& session, const std::string& root) {
    REQUIRE(session.open_project(sv(root)));
    REQUIRE(session.open_scene("stage.mcscene"));
}

u32 find_kind(const cook::BtDocument& d, StringView kind, u32 nth = 0) {
    for (u32 i = 0; i < d.nodes.size(); ++i)
        if (d.nodes[i].kind.view() == kind && nth-- == 0) return i;
    return kNoBtNode;
}

}  // namespace

TEST_CASE("bt editor: the canonical text reads back to the same document and cooks to the same tree") {
    const std::string root = bt_project("bt_ed_roundtrip");
    EditorSession session;
    open_stage(session, root);
    BtEditor ed;
    REQUIRE(ed.open(session, kTreeGuid));
    CHECK(ed.cooks());
    CHECK(ed.problems().empty());
    const cook::BtDocument& d = ed.doc();
    CHECK(d.name.view() == StringView("guard"));
    REQUIRE(d.keys.size() == 4);
    CHECK(d.keys[0].def.view() == StringView("false"));
    CHECK(d.keys[2].type.view() == StringView("vec3"));
    CHECK(d.keys[2].def.empty());
    REQUIRE(d.nodes.size() == 10);
    // Pre-order: the cook's node order.
    const char* kinds[] = {"selector", "guard", "sequence", "task", "wait", "utility", "score", "wait", "score", "emit"};
    for (u32 i = 0; i < 10; ++i) {
        CHECK(d.nodes[i].kind.view() == StringView(kinds[i]));
        CHECK(ed.cooked_index(i) == i);
        CHECK(ed.node_at(i) == i);
    }
    CHECK(d.nodes[1].field("key") == StringView("\"alert\""));
    CHECK(d.nodes[1].field("name") == StringView("\"alarm\""));
    CHECK(d.nodes[3].value.view() == StringView("\"log\""));
    CHECK(d.nodes[6].field("by") == StringView("[{\"key\": \"health\", \"to\": 100}]"));
    CHECK(d.parent(4) == 2);
    CHECK(d.parent(0) == cook::kBtNoNode);

    // The chunk is the tree's own cook, and the text reads back to the same text.
    Result<asset::BtView> view = asset::BtView::read(ed.chunk());
    REQUIRE(view);
    CHECK(view.value().nodes.size() == 10);
    const std::string a = text_of(ed.text());
    cook::JsonDoc json;
    cook::JsonError err;
    REQUIRE(json.parse(sv(a), err));
    cook::BtDocument back;
    cook::DiagList diags;
    REQUIRE(cook::read_behavior_tree_document(json.root(), "guard.mcbt", back, diags));
    cook::String b;
    cook::write_behavior_tree_document(back, kTreeGuid, b);
    CHECK(text_of(b) == a);
    Array<byte> payload;
    REQUIRE(cook::build_behavior_tree(json.root(), "guard.mcbt", payload, diags));
    REQUIRE(payload.size() == ed.chunk().size());
    CHECK(std::memcmp(payload.data(), ed.chunk().data(), payload.size()) == 0);
    // A leaf stays on one line.
    CHECK(a.find("{\"task\": \"log\", \"text\": \"alert!\"}") != std::string::npos);

    // The scene entity running it.
    Array<EntityId> users;
    ed.users(session.scene(), users);
    REQUIRE(users.size() == 1);
    CHECK(users[0] == session.scene().doc().at(0).id);
}

TEST_CASE("bt editor: node and key edits, refusals, undo and redo on disk") {
    const std::string root = bt_project("bt_ed_edits");
    EditorSession session;
    open_stage(session, root);
    BtEditor ed;
    REQUIRE(ed.open(session, kTreeGuid));
    const std::string file = root + "/assets/guard.mcbt";
    const std::string original = file_text(file);

    // A wait as the selector's first child: it is selected, the tree renumbers in pre-order.
    REQUIRE(ed.add_node(0, "wait", 0));
    CHECK(ed.doc().nodes.size() == 11);
    CHECK(ed.selected() == 1);
    CHECK(ed.doc().nodes[1].kind.view() == StringView("wait"));
    CHECK(ed.doc().nodes[1].value.view() == StringView("1"));
    CHECK(ed.dirty());
    CHECK(ed.cooks());
    // A composite comes with a placeholder child; a set takes the first key.
    REQUIRE(ed.add_node(0, "sequence"));
    const u32 seq = ed.selected();
    CHECK(ed.doc().nodes[seq].children.size() == 1);
    REQUIRE(ed.add_node(seq, "set"));
    CHECK(ed.doc().nodes[ed.selected()].value.view() == StringView("\"alert\""));
    CHECK(ed.doc().nodes[ed.selected()].field("value") == StringView("false"));

    // Refusals carry the cook's message and change nothing.
    const std::string before = text_of(ed.text());
    CHECK_FALSE(ed.add_node(1, "wait"));  // under a leaf
    CHECK_FALSE(ed.set_value(1, "\"soon\""));
    CHECK(ed.last_error().size() > 0);
    CHECK(text_of(ed.text()) == before);
    CHECK_FALSE(ed.set_field(find_kind(ed.doc(), "guard"), "key", "\"nobody\""));
    CHECK(std::string(ed.last_error().data(), ed.last_error().size()).find("nobody") != std::string::npos);
    CHECK_FALSE(ed.remove_key(0));  // "alert" is named by the guard and the set
    CHECK_FALSE(ed.remove_node(0));  // the root
    CHECK(text_of(ed.text()) == before);

    // Values and fields.
    REQUIRE(ed.set_value(1, "2.5"));
    CHECK(ed.doc().nodes[1].value.view() == StringView("2.5"));
    REQUIRE(ed.set_field(1, "name", "\"pause\""));
    CHECK(ed.doc().nodes[1].field("name") == StringView("\"pause\""));
    REQUIRE(ed.set_field(1, "name", ""));
    CHECK(ed.doc().nodes[1].field("name").empty());

    // Move, wrap, unwrap, remove.
    REQUIRE(ed.move_node(1, 1));  // past the guard
    CHECK(ed.doc().nodes[1].kind.view() == StringView("guard"));
    const u32 wait = ed.selected();
    CHECK(ed.doc().nodes[wait].kind.view() == StringView("wait"));
    CHECK(ed.doc().parent(wait) == 0);
    REQUIRE(ed.wrap_node(wait, "invert"));
    const u32 inv = ed.selected();
    CHECK(ed.doc().nodes[inv].kind.view() == StringView("invert"));
    CHECK(ed.doc().nodes[ed.doc().nodes[inv].children[0]].kind.view() == StringView("wait"));
    REQUIRE(ed.unwrap_node(inv));
    CHECK(ed.doc().nodes[ed.selected()].kind.view() == StringView("wait"));
    REQUIRE(ed.remove_node(ed.selected()));
    CHECK(ed.selected() == 0);  // the parent

    // Keys: a rename follows into the nodes; a new key with a default.
    cook::BtDocKey k = ed.doc().keys[0];
    k.name.assign("alarm_on");
    REQUIRE(ed.set_key(0, k));
    CHECK(ed.doc().nodes[1].field("key") == StringView("\"alarm_on\""));
    CHECK(ed.doc().nodes[1].field("name") == StringView("\"alarm\""));
    REQUIRE(ed.add_key("speed", "float", "2"));
    CHECK(ed.doc().keys.back().def.view() == StringView("2"));
    CHECK_FALSE(ed.add_key("speed", "int"));
    CHECK_FALSE(ed.add_key("bad", "colour"));
    REQUIRE(ed.remove_key(static_cast<u32>(ed.doc().keys.size() - 1)));

    // Wrapping the root makes the wrapper the root.
    REQUIRE(ed.wrap_node(0, "repeat"));
    CHECK(ed.doc().nodes[0].kind.view() == StringView("repeat"));
    CHECK(ed.doc().nodes[1].kind.view() == StringView("selector"));
    CHECK(ed.selected() == 0);

    // One undo step for the whole run; saved on disk; redo brings it back.
    REQUIRE(ed.save(session));
    CHECK_FALSE(ed.dirty());
    const std::string edited = file_text(file);
    CHECK(edited != original);
    CHECK(edited == text_of(ed.text()));
    REQUIRE(ed.undo(session));
    CHECK(ed.doc().nodes.size() == 10);
    CHECK(ed.doc().nodes[0].kind.view() == StringView("selector"));
    CHECK(file_text(file) == text_of(ed.text()));
    REQUIRE(ed.redo(session));
    CHECK(ed.doc().nodes[0].kind.view() == StringView("repeat"));
    CHECK(file_text(file) == edited);

    // A file changed elsewhere is read again.
    cook::String other;
    cook::write_behavior_tree_document(ed.doc(), kTreeGuid, other);
    std::string changed = text_of(other);
    const usize at = changed.find("\"guard\"");
    REQUIRE(at != std::string::npos);
    changed.replace(at, 7, "\"guard2\"");
    REQUIRE(session.write_asset("guard.mcbt", sv(changed)));
    REQUIRE(ed.sync(session));
    CHECK(ed.doc().name.view() == StringView("guard2"));
}

TEST_CASE("bt editor: a tree that does not cook opens with its problems and is fixed in place") {
    const char* broken = R"({ "format": "mce-bt", "version": 1, "guid": "7e500000-0000-4000-8000-000000000002",
      "blackboard": { "alert": "bool" },
      "root": { "sequence": [ { "check": "missing" }, { "wait": 1 } ] } })";
    const std::string root = bt_project("bt_ed_broken", broken);
    EditorSession session;
    open_stage(session, root);
    BtEditor ed;
    REQUIRE(ed.open(session, kTreeGuid));
    CHECK_FALSE(ed.cooks());
    REQUIRE(ed.problems().size() >= 1);
    CHECK(std::string(ed.problems()[0].view().data(), ed.problems()[0].size()).find("missing") != std::string::npos);
    CHECK(ed.chunk().empty());
    // Edits that still do not cook are kept while it does not cook; the fix makes it cook.
    REQUIRE(ed.set_value(2, "3"));
    CHECK_FALSE(ed.cooks());
    REQUIRE(ed.set_value(1, "\"alert\""));
    CHECK(ed.cooks());
    CHECK(ed.problems().empty());
    // From then on, a breaking edit is refused.
    CHECK_FALSE(ed.set_value(1, "\"missing\""));

    // A file that is not a tree does not open.
    BtEditor none;
    CHECK_FALSE(none.open(session, asset::Guid{}));
}

TEST_CASE("bt editor: create writes a tree that cooks") {
    const std::string root = bt_project("bt_ed_create");
    EditorSession session;
    open_stage(session, root);
    BtEditor ed;
    Result<asset::Guid> g = ed.create(session, "ai/new.mcbt");
    REQUIRE(g);
    CHECK(ed.is_open());
    CHECK(ed.cooks());
    CHECK(ed.doc().nodes.size() == 2);
    CHECK_FALSE(ed.create(session, "ai/new.mcbt"));
    CHECK_FALSE(ed.create(session, "ai/new.json"));
}

TEST_CASE("bt editor: the debugger reads a playing brain and runs unsaved edits") {
    const std::string root = bt_project("bt_ed_debug");
    EditorSession session;
    open_stage(session, root);
    BtEditor ed;
    REQUIRE(ed.open(session, kTreeGuid));

    Array<Entity> brains;
    bt_play_brains(session, ed, brains);
    CHECK(brains.empty());  // not playing
    REQUIRE(session.enter_play());
    session.play().pause();
    for (int i = 0; i < 4; ++i) session.play().step();
    bt_play_brains(session, ed, brains);
    REQUIRE(brains.size() == 1);

    BtLive live;
    REQUIRE(bt_live(session, ed, brains[0], live));
    CHECK(live.current);
    CHECK(live.active);
    REQUIRE(live.status.size() == 10);
    CHECK(live.status[0] == BtStatus::Running);  // selector
    CHECK(live.status[1] == BtStatus::Failure);  // the guard: alert is false
    CHECK(live.status[5] == BtStatus::Running);  // utility
    CHECK(live.status[6] == BtStatus::Running);  // the better score
    CHECK(live.status[7] == BtStatus::Running);  // its wait
    CHECK(live.score[6] == doctest::Approx(0.6f));
    CHECK(live.score[8] == doctest::Approx(0.2f));
    CHECK(std::isnan(live.score[0]));
    REQUIRE(live.keys.size() == 4);
    CHECK(live.keys[0].set);
    CHECK(live.keys[0].value.view() == StringView("false"));
    CHECK(live.keys[1].value.view() == StringView("50"));
    CHECK_FALSE(live.keys[2].set);

    // The blackboard moves the tree: with alert set, the guard's branch runs.
    REQUIRE(kit::bb_set_number(*session.play().world(), brains[0], "alert", 1.0f));
    session.play().step();
    REQUIRE(bt_live(session, ed, brains[0], live));
    CHECK(live.status[1] == BtStatus::Running);
    CHECK(live.keys[0].value.view() == StringView("true"));

    // An unsaved node edit: the play world still runs the old tree until it is pushed.
    REQUIRE(ed.add_node(0, "wait", 0));
    REQUIRE(bt_live(session, ed, brains[0], live));
    CHECK_FALSE(live.current);
    CHECK(live.status[0] == BtStatus::Idle);
    REQUIRE(bt_push_to_play(session, ed));
    session.play().step();
    session.play().step();
    REQUIRE(bt_live(session, ed, brains[0], live));
    CHECK(live.current);
    CHECK(live.status[1] == BtStatus::Running);  // the new first wait
    CHECK(ed.dirty());  // pushing does not save

    session.stop_play();
    CHECK_FALSE(bt_live(session, ed, brains[0], live));
    CHECK_FALSE(bt_push_to_play(session, ed));
}
