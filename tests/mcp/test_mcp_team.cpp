// Several agents on one project (ROADMAP M-AGENT step 7): two servers on one copy of the scene_demo
// project, the way two agents each run their own. A scene both edit merges by entity, a locked file
// refuses the other's writes, a task is never claimed twice, and the event feed shows who did what.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <string>

#include "mcp_client.h"
#include "mce/mcp/json_writer.h"
#include "tools_team.h"

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

mcp::ServerOptions named(const char* agent) {
    mcp::ServerOptions o;
    o.agent = agent;
    return o;
}

struct Agent : Client {
    Agent(const std::string& root, const char* name) : Client(named(name)) {
        REQUIRE(server.open_project(sv(root), "scenes/demo.mcscene"));
        init();
    }
};

// The saved scene's entity named `name` as compact JSON ("" when there is none), and its entity count.
std::string entity(const std::string& root, const std::string& name, u32* count = nullptr) {
    auto p = parse(read_file(root + "/assets/scenes/demo.mcscene"));
    const cook::JsonValue ents = p->root().get("entities");
    if (count) *count = ents.size();
    for (u32 i = 0; i < ents.size(); ++i)
        if (str(ents.at(i).get("name")) == name) {
            cook::String s;
            mcp::JsonWriter w(s);
            w.copy(ents.at(i));
            return std::string(s.c_str());
        }
    return {};
}

std::string feed(Client& c, const std::string& args = "{}") {
    std::string s;
    auto t = c.ok("project_watch", args);
    const cook::JsonValue ev = t.v().get("events");
    for (u32 i = 0; i < ev.size(); ++i)
        s += str(ev.at(i).get("agent")) + " " + str(ev.at(i).get("tool")) + " " + str(ev.at(i).get("path")) + "\n";
    return s;
}

}  // namespace

TEST_CASE("scene_save: another agent's save is merged in by entity; new ids that clash are renumbered") {
    const std::string root = demo_project("mcp_team_merge");
    Agent alice(root, "alice");
    Agent bob(root, "bob");
    CHECK(str(alice.ok("project_watch").v().get("agent")) == "alice");

    alice.ok("component_set", R"({"entity":"Crate A","component":"LocalTransform","path":"position","value":[5,5,0]})");
    alice.ok("entity_create", R"({"name":"Gem"})");
    bob.ok("component_set", R"({"entity":"Ball","component":"Collider2D","path":"restitution","value":0.9})");
    bob.ok("entity_create", R"({"name":"Coin"})");
    bob.ok("entity_delete", R"({"entity":"Crate B"})");

    alice.ok("scene_save");
    auto merged = bob.ok("scene_save");  // alice saved meanwhile: merged, not refused
    u32 count = 0;
    CHECK(entity(root, "Crate A", &count).find("[5,5,0]") != std::string::npos);  // alice's edit survived bob's save
    CHECK(entity(root, "Ball").find("0.9") != std::string::npos);
    CHECK(entity(root, "Crate B").empty());
    CHECK_FALSE(entity(root, "Gem").empty());
    CHECK_FALSE(entity(root, "Coin").empty());
    CHECK(count == 7);  // 6 - Crate B + Gem + Coin
    CHECK(entity(root, "Gem").find("\"id\":7") != std::string::npos);  // alice's, saved first, keeps 7
    CHECK(entity(root, "Coin").find("\"id\":8") != std::string::npos);  // bob's moved off it

    // Bob's open scene is the merge: alice sees bob's edits after a reload, bob sees alice's now.
    CHECK(bob.ok("scene_get_entity", R"({"entity":"Crate A"})").text.find("\"position\": [5, 5, 0]") != std::string::npos);
    CHECK(feed(alice, R"({"others":true})").find("bob scene_merge scenes/demo.mcscene") != std::string::npos);
    CHECK(feed(alice).find("alice scene_save scenes/demo.mcscene") != std::string::npos);

    // Saving again with nothing new from anyone is a plain save.
    bob.ok("component_set", R"({"entity":"Ball","component":"Collider2D","path":"restitution","value":0.5})");
    bob.ok("scene_save");
    CHECK(entity(root, "Ball").find("0.5") != std::string::npos);
}

TEST_CASE("scene_save: both agents changing one component conflict; nothing is lost until resolve picks a side") {
    const std::string root = demo_project("mcp_team_conflict");
    Agent alice(root, "alice");
    Agent bob(root, "bob");
    alice.ok("component_set", R"({"entity":"Crate B","component":"LocalTransform","path":"position","value":[1,1,0]})");
    bob.ok("component_set", R"({"entity":"Crate B","component":"LocalTransform","path":"position","value":[2,2,0]})");
    bob.ok("component_set", R"({"entity":"Ball","component":"Collider2D","path":"restitution","value":0.9})");
    alice.ok("scene_save");
    const std::string err = bob.fails("scene_save");
    CHECK(err.find("conflict") != std::string::npos);
    CHECK(err.find("\"Crate B\" components/LocalTransform: both changed it") != std::string::npos);
    CHECK(entity(root, "Crate B").find("[1,1,0]") != std::string::npos);  // nothing written
    CHECK(bob.fails("scene_save", R"({"resolve":"mine"})").find("'resolve'") != std::string::npos);

    bob.ok("scene_save", R"({"resolve":"ours"})");
    CHECK(entity(root, "Crate B").find("[2,2,0]") != std::string::npos);
    CHECK(entity(root, "Ball").find("0.9") != std::string::npos);

    // Now alice's open copy is behind: her next edit elsewhere merges and keeps bob's Crate B.
    alice.ok("component_set", R"({"entity":"Title","component":"TextLabel","path":"text","value":"HELLO"})");
    alice.ok("scene_save");
    CHECK(entity(root, "Crate B").find("[2,2,0]") != std::string::npos);
    CHECK(entity(root, "Title").find("HELLO") != std::string::npos);
}

TEST_CASE("merge_entity_files: references inside renumbered entities follow them") {
    const char* base = R"({"format":"mce-scene","version":1,"name":"s","entities":[{"id":1,"name":"A"}]})";
    const char* ours =
        R"({"format":"mce-scene","version":1,"name":"s","entities":[{"id":1,"name":"A"},{"id":2,"name":"Mine","parent":"$1"},{"id":3,"name":"Kid","components":{"Follow":{"target":"$2"}}}]})";
    const char* theirs = R"({"format":"mce-scene","version":1,"name":"s","entities":[{"id":1,"name":"A2"},{"id":2,"name":"Theirs"}]})";
    mcp::MergeResult res;
    cook::String err;
    REQUIRE(mcp::merge_entity_files(base, ours, theirs, "", res, err));
    CHECK(res.conflicts.empty());
    CHECK(res.renumbered >= 1);
    const std::string text = res.text.c_str();
    CHECK(text.find(R"("name":"A2")") != std::string::npos);
    CHECK(text.find(R"("id":2,"name":"Theirs")") != std::string::npos);
    // Mine left id 2 to theirs; Kid's reference follows it.
    const usize mine = text.find(R"("name":"Mine")");
    REQUIRE(mine != std::string::npos);
    const std::string mine_id = text.substr(text.rfind("\"id\":", mine) + 5, 1);
    CHECK(mine_id != "2");
    CHECK(text.find("\"target\":\"$" + mine_id + "\"") != std::string::npos);
    CHECK_FALSE(mcp::merge_entity_files(base, "not json", theirs, "", res, err));
}

TEST_CASE("asset_lock: the other agent's writes to a locked file are refused until it is unlocked or expires") {
    const std::string root = demo_project("mcp_team_locks");
    Agent alice(root, "alice");
    Agent bob(root, "bob");
    auto l = alice.ok("asset_lock", R"({"path":"scenes/demo.mcscene","note":"reworking the level"})");
    CHECK(str(l.v().get("lock").get("agent")) == "alice");
    alice.ok("asset_lock", R"({"path":"scenes/demo.mcscene"})");  // refreshing your own lock is fine
    CHECK(bob.fails("asset_lock", R"({"path":"scenes/demo.mcscene"})").find("already locked by alice") != std::string::npos);
    CHECK(bob.fails("asset_lock", R"({"path":"../x"})").find("'path'") != std::string::npos);

    bob.ok("component_set", R"({"entity":"Ball","component":"Collider2D","path":"restitution","value":0.9})");
    const std::string refused = bob.fails("scene_save");
    CHECK(refused.find("assets/scenes/demo.mcscene is locked by alice (reworking the level)") != std::string::npos);
    CHECK(bob.fails("asset_set_import_settings", R"({"path":"scenes/demo.mcscene","settings":{}})").find("locked by alice") != std::string::npos);
    alice.ok("scene_save");  // the holder writes

    auto locks = bob.ok("asset_locks");
    REQUIRE(locks.v().get("locks").size() == 1);
    CHECK(str(locks.v().get("locks").at(0).get("path")) == "scenes/demo.mcscene");
    CHECK(bob.fails("asset_unlock", R"({"path":"scenes/demo.mcscene"})").find("alice's") != std::string::npos);
    alice.ok("asset_unlock", R"({"path":"scenes/demo.mcscene"})");
    CHECK(alice.fails("asset_unlock", R"({"path":"scenes/demo.mcscene"})").find("not locked") != std::string::npos);
    bob.ok("scene_save");
    CHECK(entity(root, "Ball").find("0.9") != std::string::npos);

    // An expired lock is taken over; force breaks a live one, and the feed says so.
    write_file(root + "/.mce/locks/scenes%demo.mcscene.lock",
               R"({"path":"scenes/demo.mcscene","agent":"carol","since":1000,"ttl":10,"note":""})");
    CHECK(bob.ok("asset_lock", R"({"path":"scenes/demo.mcscene"})").v().get("took_over").as_bool());
    alice.ok("asset_unlock", R"({"path":"scenes/demo.mcscene","force":true})");
    CHECK(feed(bob).find("alice asset_unlock scenes/demo.mcscene") != std::string::npos);
}

TEST_CASE("tasks: a shared list in tasks.mctasks; claims respect dependencies and are never granted twice") {
    const std::string root = demo_project("mcp_team_tasks");
    Agent alice(root, "alice");
    Agent bob(root, "bob");
    CHECK(alice.ok("task_list").v().get("tasks").size() == 0);
    CHECK(alice.ok("task_add", R"({"title":"Draw the player sprite","files":["sprites/player.png"]})").v().get("task").get("id").as_number() == 1);
    CHECK(alice.ok("task_add", R"({"title":"Animate the player","depends":[1]})").v().get("task").get("id").as_number() == 2);
    CHECK(alice.fails("task_add", R"({"title":"x","depends":[9]})").find("'depends'") != std::string::npos);
    CHECK(alice.fails("task_add", R"({"title":""})").find("'title'") != std::string::npos);

    auto claimed = bob.ok("task_claim");
    CHECK(claimed.v().get("task").get("id").as_number() == 1);
    CHECK(str(claimed.v().get("task").get("owner")) == "bob");
    CHECK(alice.fails("task_claim").find("no open task whose dependencies are done") != std::string::npos);
    CHECK(alice.fails("task_claim", R"({"id":1})").find("already claimed by bob") != std::string::npos);
    CHECK(alice.fails("task_claim", R"({"id":2})").find("depends on tasks that are not done") != std::string::npos);
    CHECK(alice.fails("task_update", R"({"id":1,"status":"done"})").find("claimed by bob") != std::string::npos);
    alice.ok("task_update", R"({"id":1,"notes":"use the slimes palette"})");  // notes from anyone

    auto done = bob.ok("task_update", R"({"id":1,"status":"done","notes":"saved"})");
    CHECK(str(done.v().get("task").get("notes")) == "alice: use the slimes palette\nbob: saved");
    CHECK(alice.ok("task_claim").v().get("task").get("id").as_number() == 2);
    auto list = bob.ok("task_list");
    CHECK(list.v().get("done").as_number() == 1);
    CHECK(list.v().get("claimed").as_number() == 1);
    CHECK(bob.ok("task_list", R"({"status":"done"})").v().get("tasks").size() == 1);
    CHECK(read_file(root + "/tasks.mctasks").find("\"format\":\"mce-tasks\"") != std::string::npos);

    // project_watch pages: next is the since for the next call.
    auto w = bob.ok("project_watch", R"({"limit":2})");
    CHECK(w.v().get("events").size() == 2);
    const u64 next = static_cast<u64>(w.v().get("next").as_number());
    CHECK(next == 2);
    auto rest = bob.ok("project_watch", "{\"since\":" + std::to_string(next) + "}");
    CHECK(str(rest.v().get("events").at(0).get("tool")) == "task_claim");
    CHECK(feed(bob, R"({"others":true})").find("bob ") == std::string::npos);
}

TEST_CASE("scene_switch: several scenes open at once, each keeping its unsaved edits") {
    const std::string root = demo_project("mcp_team_switch");
    std::filesystem::copy_file(root + "/assets/scenes/demo.mcscene", root + "/assets/scenes/menu.mcscene");
    std::string menu = read_file(root + "/assets/scenes/menu.mcscene");
    menu.replace(menu.find("5ce4e000-0000-4000-8000-000000000100"), 36, "5ce4e000-0000-4000-8000-0000000001ff");
    write_file(root + "/assets/scenes/menu.mcscene", menu);
    Agent a(root, "alice");
    a.ok("cook_run", R"({"adopt":true})");
    a.ok("component_set", R"({"entity":"Ball","component":"Collider2D","path":"restitution","value":0.9})");
    auto s = a.ok("scene_switch", R"({"path":"scenes/menu.mcscene"})");
    CHECK_FALSE(s.v().get("restored_edits").as_bool());
    CHECK(s.v().get("open").size() == 2);
    a.ok("entity_delete", R"({"entity":"Ball"})");
    a.ok("scene_save");
    auto back = a.ok("scene_switch", R"({"path":"scenes/demo.mcscene"})");
    CHECK(back.v().get("restored_edits").as_bool());
    CHECK(a.ok("scene_get_entity", R"({"entity":"Ball"})").text.find("0.9") != std::string::npos);
    auto docs = a.ok("scene_documents");
    CHECK(docs.v().get("open").size() == 2);
    CHECK(str(docs.v().get("open").at(0).get("path")) == "scenes/demo.mcscene");
    CHECK(docs.v().get("open").at(0).get("dirty").as_bool());
    CHECK_FALSE(docs.v().get("open").at(1).get("dirty").as_bool());
    a.ok("scene_save");
    CHECK(entity(root, "Ball").find("0.9") != std::string::npos);
    CHECK(read_file(root + "/assets/scenes/menu.mcscene").find("\"Ball\"") == std::string::npos);
    CHECK(a.fails("scene_switch", R"({"path":"scenes/nope.mcscene"})").size() > 0);
}

TEST_CASE("report_friction: notes from several agents append to the project's friction.jsonl") {
    const std::string root = demo_project("mcp_team_friction");
    Agent alice(root, "alice");
    Agent bob(root, "bob");
    auto a = alice.ok("report_friction",
                      R"({"summary":"render_frame cannot show one entity alone","area":"render_frame","severity":"minor","workaround":"hid the rest"})");
    CHECK(a.v().get("id").as_number() == 1);
    CHECK(a.v().get("same_area").as_number() == 0);
    auto b = bob.ok("report_friction",
                    R"({"summary":"no tool reorders entities","area":"render_frame","tool":"entity_create","expected":"a move","actual":"recreate"})");
    CHECK(b.v().get("id").as_number() == 2);
    CHECK(b.v().get("same_area").as_number() == 1);
    const std::string text = read_file(root + "/friction.jsonl");
    CHECK(text.find(R"("agent":"alice","server":)") != std::string::npos);
    CHECK(text.find(R"("severity":"major","area":"render_frame","summary":"no tool reorders entities","tool":"entity_create")") != std::string::npos);
    CHECK(text.find(R"("scene":"scenes/demo.mcscene")") != std::string::npos);
    CHECK(std::count(text.begin(), text.end(), '\n') == 2);

    CHECK(alice.fails("report_friction", R"({"summary":"x","area":"docs","severity":"awful"})").find("'severity'") != std::string::npos);
    CHECK(alice.fails("report_friction", R"({"summary":"x","area":"docs","severity":"blocking"})").find("'actual'") != std::string::npos);
    CHECK(alice.fails("report_friction", R"({"summary":"","area":"docs"})").find("'summary'") != std::string::npos);
    CHECK(alice.fails("report_friction", R"({"summary":"x"})").find("area") != std::string::npos);
}
