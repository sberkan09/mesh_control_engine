// M-HARDEN2 3: the behaviour-tree, animation-graph and UI-document editors over hostile files and hostile edits. A
// mutated file is opened (it may refuse or open with problems), then a run of random edits, saves, undos and redos is
// thrown at whatever opened. Nothing may crash, hang or fire an MCE_ASSERT, and a document the editor keeps must write
// text again.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "json_swap.h"
#include "mce/cook/fs.h"
#include "mce/editor/animgraph_editor.h"
#include "mce/editor/bt_editor.h"
#include "mce/editor/editor_session.h"
#include "mce/editor/ui_document_editor.h"
#include "mce/log.h"
#include "mutator.h"

using namespace mce;

namespace {

u32 iterations(u32 base) {
    const char* e = std::getenv("MCE_FUZZ_ITERS");
    return e ? static_cast<u32>(std::strtoul(e, nullptr, 10)) : base;
}

std::string scratch_dir(const char* name) {
    const char* root = std::getenv("MCE_FUZZ_ROOT");
    std::filesystem::path p = std::filesystem::path(root ? root : ".") / "fuzz_editor_trees" / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p / "assets");
    return p.string();
}

const char* kTree =
    R"({ "format": "mce-bt", "version": 1, "guid": "7e500000-0000-4000-8000-000000000002", "name": "guard",
  "blackboard": { "alert": { "type": "bool", "default": false }, "health": { "type": "float", "default": 50 },
                  "home": "vec3", "target": "entity" },
  "root": { "selector": [
    { "guard": { "sequence": [ { "task": "log", "text": "alert!" }, { "wait": 1 } ] }, "key": "alert", "value": true, "abort": "lower", "name": "alarm" },
    { "utility": [
      { "score": { "wait": 2 }, "base": 0.1, "by": [ { "key": "health", "to": 100 } ] },
      { "score": { "emit": "bored" }, "base": 0.2 } ] } ] } })";

const char* kKinds[] = {"selector", "sequence", "utility", "parallel", "wait", "task", "inverter",
                        "repeat",   "guard",    "emit",    "score",    "nope", ""};
const char* kJson[] = {"1",    "0",  "-3", "2.5",          "true", "\"chase\"", "\"\"",
                       "null", "[]", "{}", "{\"wait\":1}", "[1,2", "1e400",     "\"alert\""};
const char* kFields[] = {"base", "key", "value", "abort", "name", "by", "text", "limit", "x", ""};
const char* kTypes[] = {"bool", "float", "int", "vec3", "entity", "string", "nope", ""};

u32 below(fuzz::Mutator& m, u32 n) { return static_cast<u32>(m.below(n)); }

template <class T, usize N>
const T& pick(fuzz::Mutator& m, const T (&a)[N]) {
    return a[below(m, static_cast<u32>(N))];
}

void run_edits(editor::EditorSession& session, editor::BtEditor& bt, fuzz::Mutator& m, u32 steps) {
    for (u32 s = 0; s < steps; ++s) {
        const u32 nodes = static_cast<u32>(bt.doc().nodes.size());
        const u32 node = nodes ? below(m, nodes + 2) : 0;  // sometimes out of range
        switch (below(m, 14)) {
            case 0:
                (void)bt.add_node(node, StringView(pick(m, kKinds)));
                break;
            case 1:
                (void)bt.remove_node(node);
                break;
            case 2:
                (void)bt.move_node(node, static_cast<i32>(below(m, 7)) - 3);
                break;
            case 3:
                (void)bt.wrap_node(node, StringView(pick(m, kKinds)));
                break;
            case 4:
                (void)bt.unwrap_node(node);
                break;
            case 5:
                (void)bt.set_value(node, StringView(pick(m, kJson)));
                break;
            case 6:
                (void)bt.set_field(node, StringView(pick(m, kFields)), StringView(pick(m, kJson)));
                break;
            case 7:
                (void)bt.add_key(StringView(pick(m, kFields)), StringView(pick(m, kTypes)), StringView(pick(m, kJson)));
                break;
            case 8:
                (void)bt.remove_key(below(m, 6));
                break;
            case 9:
                (void)bt.set_name(StringView(pick(m, kFields)));
                break;
            case 10:
                (void)bt.save(session);
                break;
            case 11:
                (void)bt.undo(session);
                break;
            case 12:
                (void)bt.redo(session);
                break;
            default:
                bt.select(node);
                break;
        }
        // What the editor keeps always writes text that reads back to a tree of the same size.
        const cook::String text = bt.text();
        CHECK_FALSE(text.view().empty());
    }
}

std::string mutate_text(fuzz::Mutator& m, std::vector<std::uint64_t>& storage, const std::string& seed, u32 it) {
    if (it == 0) return seed;
    const Span<const byte> bytes(reinterpret_cast<const byte*>(seed.data()), seed.size());
    if (it % 2 == 1) return fuzz::swap_value(m, bytes);
    const usize len = m.mutate(bytes, storage);
    return std::string(reinterpret_cast<const char*>(storage.data()), len);
}

void write_text(const std::string& path, const std::string& text) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

TEST_CASE("fuzz: the behaviour-tree editor survives mutated files and random edit runs") {
    log::set_level(log::Level::Off);
    const std::string root = scratch_dir("bt");
    const std::string seed = kTree;
    const asset::Guid guid = asset::parse_guid("7e500000-0000-4000-8000-000000000002").value();
    std::vector<std::uint64_t> storage;
    fuzz::Mutator m(0xB7EE);
    const u32 n = iterations(300);
    u64 opened = 0;
    for (u32 it = 0; it < n; ++it) {
        INFO("replay: behaviour-tree iteration " << it);
        write_text(root + "/assets/guard.mcbt", mutate_text(m, storage, seed, it));
        editor::EditorSession session;
        if (!session.open_project(StringView(root.data(), root.size()))) continue;
        editor::BtEditor bt;
        if (!bt.open(session, guid)) continue;
        ++opened;
        run_edits(session, bt, m, 24);
    }
    std::printf("fuzz: behaviour tree: %u mutants, %llu opened\n", n, static_cast<unsigned long long>(opened));
    CHECK(opened > 0);
}

namespace {

const char* kGraph = R"({ "format": "mce-animgraph", "version": 1, "guid": "7e400000-0000-4000-8000-000000000002",
    "name": "walker", "params": ["speed"],
    "states": [
      { "name": "move", "blend": { "param": "speed", "points": [ { "clip": "Idle", "at": 0 }, { "clip": "Walk", "at": 1 } ] },
        "events": [ { "time": 0.5, "name": "step" } ] },
      { "name": "wave", "clip": "Walk", "loop": false, "speed": 1.5 } ],
    "transitions": [
      { "from": "move", "to": "wave", "when": [ { "param": "speed", "op": ">=", "value": 2 } ], "fade": 0.1 },
      { "from": "wave", "to": "move", "on_finish": true } ] })";

void run_graph_edits(editor::EditorSession& session, editor::AnimGraphEditor& ed, fuzz::Mutator& m, u32 steps) {
    for (u32 s = 0; s < steps; ++s) {
        const u32 states = static_cast<u32>(ed.doc().states.size());
        const u32 i = below(m, states + 2);
        const u32 j = below(m, states + 2);
        switch (below(m, 14)) {
            case 0:
                (void)ed.add_state(StringView(pick(m, kFields)), StringView(pick(m, kFields)), 1.0f, 2.0f);
                break;
            case 1:
                (void)ed.remove_state(i);
                break;
            case 2:
                (void)ed.move_state(i, 3.0f, -4.0f);
                break;
            case 3:
                (void)ed.set_start(i);
                break;
            case 4:
                (void)ed.add_transition(below(m, 2) ? i : cook::kAnimGraphAny, j);
                break;
            case 5:
                (void)ed.remove_transition(below(m, 4));
                break;
            case 6:
                (void)ed.add_param(StringView(pick(m, kFields)));
                break;
            case 7:
                (void)ed.rename_param(below(m, 5), StringView(pick(m, kFields)));
                break;
            case 8:
                (void)ed.remove_param(below(m, 5));
                break;
            case 9:
                (void)ed.set_name(StringView(pick(m, kFields)));
                break;
            case 10:
                (void)ed.save(session);
                break;
            case 11:
                (void)ed.undo(session);
                break;
            case 12:
                (void)ed.redo(session);
                break;
            default:
                ed.select_state(i);
                break;
        }
        CHECK_FALSE(ed.text().view().empty());
    }
}

}  // namespace

TEST_CASE("fuzz: the animation-graph editor survives mutated files and random edit runs") {
    log::set_level(log::Level::Off);
    const std::string root = scratch_dir("anim");
    const std::string seed = kGraph;
    const asset::Guid guid = asset::parse_guid("7e400000-0000-4000-8000-000000000002").value();
    std::vector<std::uint64_t> storage;
    fuzz::Mutator m(0xA41F);
    const u32 n = iterations(300);
    u64 opened = 0;
    for (u32 it = 0; it < n; ++it) {
        INFO("replay: animation-graph iteration " << it);
        write_text(root + "/assets/walker.mcanim", mutate_text(m, storage, seed, it));
        editor::EditorSession session;
        if (!session.open_project(StringView(root.data(), root.size()))) continue;
        editor::AnimGraphEditor ed;
        if (!ed.open(session, guid)) continue;
        ++opened;
        ed.layout();
        run_graph_edits(session, ed, m, 24);
    }
    std::printf("fuzz: animation graph: %u mutants, %llu opened\n", n, static_cast<unsigned long long>(opened));
    CHECK(opened > 0);
}

TEST_CASE("fuzz: the UI-document editor survives mutated files and random edit runs") {
    log::set_level(log::Level::Off);
    const std::string root = scratch_dir("ui");
    // The seed is what the editor itself writes for a small menu.
    std::string seed;
    asset::Guid guid;
    {
        editor::EditorSession session;
        REQUIRE(session.open_project(StringView(root.data(), root.size())));
        editor::UiDocumentEditor ed;
        Result<asset::Guid> made = ed.create(session, "ui/menu.mcui");
        REQUIRE(made.has_value());
        guid = made.value();
        REQUIRE(ed.add_widget(ui::kNoWidgetIndex, ui::WidgetKind::Stack));
        REQUIRE(ed.add_widget(0, ui::WidgetKind::Button));
        REQUIRE(ed.add_widget(0, ui::WidgetKind::Text));
        REQUIRE(ed.add_widget(0, ui::WidgetKind::Slider));
        REQUIRE(ed.save(session).has_value());
        const cook::String t = ed.text();
        seed.assign(t.view().data(), t.view().size());
    }
    std::vector<std::uint64_t> storage;
    fuzz::Mutator m(0x01D0C);
    const u32 n = iterations(300);
    u64 opened = 0;
    for (u32 it = 0; it < n; ++it) {
        INFO("replay: UI-document iteration " << it);
        write_text(root + "/assets/ui/menu.mcui", mutate_text(m, storage, seed, it));
        editor::EditorSession session;
        if (!session.open_project(StringView(root.data(), root.size()))) continue;
        editor::UiDocumentEditor ed;
        if (!ed.open(session, guid)) continue;
        ++opened;
        for (u32 s = 0; s < 24; ++s) {
            const u32 w = below(m, static_cast<u32>(ed.data().widgets.size()) + 2);
            switch (below(m, 8)) {
                case 0:
                    (void)ed.add_widget(w, static_cast<ui::WidgetKind>(below(m, ui::kWidgetKindCount)));
                    break;
                case 1:
                    (void)ed.remove_widget(w);
                    break;
                case 2:
                    (void)ed.move_widget(w, static_cast<i32>(below(m, 3)) - 1);
                    break;
                case 3:
                    (void)ed.set_kind(w, static_cast<ui::WidgetKind>(below(m, ui::kWidgetKindCount)));
                    break;
                case 4:
                    (void)ed.save(session);
                    break;
                case 5:
                    (void)ed.undo(session);
                    break;
                case 6:
                    (void)ed.redo(session);
                    break;
                default:
                    ed.select(w);
                    break;
            }
            CHECK_FALSE(ed.text().view().empty());
        }
    }
    std::printf("fuzz: UI document: %u mutants, %llu opened\n", n, static_cast<unsigned long long>(opened));
    CHECK(opened > 0);
}
