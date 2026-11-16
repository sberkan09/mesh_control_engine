// The UI Document panel's editor (ROADMAP M-UI-DATA step 6): building a document widget by widget,
// the cook's read-back refusing a bad edit, moves and removals keeping nav and focus right, undo and
// redo, the inspector through a scripted DrawCtx, and the saved file being the canonical text the
// cook reads to the same document.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <string>

#include "cook_test_util.h"
#include "mce/cook/fs.h"
#include "mce/cook/json.h"
#include "mce/cook/ui_text.h"
#include "mce/editor/editor_session.h"
#include "mce/editor/ui_document_editor.h"

using namespace mce;
using namespace mce::editor;
using namespace cook_test;

namespace {

StringView sv(const std::string& s) { return StringView(s.data(), s.size()); }

std::string ui_project(const char* name) {
    const std::string root = fresh_dir(name);
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    return root;
}

std::string file_text(const std::string& path) {
    cook::String s;
    REQUIRE(cook::fs::read_text(sv(path), s));
    return std::string(s.view().data(), s.view().size());
}

ui::DocumentData read_doc(const std::string& text) {
    cook::JsonDoc doc;
    cook::JsonError err;
    REQUIRE(doc.parse(sv(text), err));
    ui::DocumentData d;
    cook::DiagList diags;
    REQUIRE(cook::read_ui_document(doc.root(), "x.mcui", d, diags));
    return d;
}

std::string path_of(const UiDocumentEditor& ed, u32 i) {
    const cook::String p = ed.widget_path(i);
    return std::string(p.view().data(), p.view().size());
}

// Answers edits keyed "<group>/<label>" (the innermost group) or "<label>".
class ScriptCtx final : public DrawCtx {
public:
    std::map<std::string, std::function<void(void*)>> edits;
    std::vector<std::string> seen;

    bool begin_component(const char*, bool*) override { return true; }
    void end_component() override {}
    bool begin_group(const char* label, bool) override {
        groups_.push_back(label);
        return true;
    }
    void end_group() override { groups_.pop_back(); }
    bool text_field(const char* l, char* buf, usize cap) override {
        return hit(l, [&](auto& f) {
            std::string v;
            f(&v);
            std::snprintf(buf, cap, "%s", v.c_str());
        });
    }
    bool checkbox(const char* l, bool* v) override {
        return hit(l, [&](auto& f) { f(v); });
    }
    bool int_field(const char* l, i64* v, i64, i64) override {
        return hit(l, [&](auto& f) { f(v); });
    }
    bool float_field(const char* l, f64* v, f64, f64) override {
        return hit(l, [&](auto& f) { f(v); });
    }
    bool float_n(const char* l, f32* v, u32, f32, f32) override {
        return hit(l, [&](auto& f) { f(v); });
    }
    bool color_field(const char* l, f32* v) override {
        return hit(l, [&](auto& f) { f(v); });
    }
    bool enum_field(const char* l, i64* v, const refl::EnumRecord&) override {
        return hit(l, [&](auto& f) { f(v); });
    }
    bool asset_field(const char* l, asset::Guid* g, StringView) override {
        return hit(l, [&](auto& f) { f(g); });
    }
    bool entity_field(const char*, EntityId*) override { return false; }
    void read_only(const char* l, StringView) override { seen.push_back(key(l)); }
    void help(const char*) override {}
    const refl::TypeRecord* add_component_menu(Span<const refl::TypeRecord* const>) override { return nullptr; }

private:
    std::string key(const char* label) const { return groups_.empty() ? label : groups_.back() + "/" + label; }
    template <class Fn>
    bool hit(const char* label, Fn&& fn) {
        const std::string k = key(label);
        seen.push_back(k);
        auto it = edits.find(k);
        if (it == edits.end()) return false;
        fn(it->second);
        return true;
    }
    std::vector<std::string> groups_;
};

}  // namespace

TEST_CASE("UI document editor: build, refuse, move, remove, undo; the file is the cook's canonical text") {
    const std::string root = ui_project("ui_document_editor");
    EditorSession session;
    REQUIRE(session.open_project(root).has_value());
    UiDocumentEditor ed;
    Result<asset::Guid> made = ed.create(session, "ui/title.mcui");
    REQUIRE(made.has_value());
    CHECK(ed.is_open());
    CHECK(ed.data().widgets.empty());
    CHECK_FALSE(ed.create(session, "ui/title.mcui").has_value());  // exists
    CHECK_FALSE(ed.create(session, "ui/title.txt").has_value());
    const std::string file = root + "/assets/ui/title.mcui";

    // A stack with a title and two buttons; names are free per parent.
    REQUIRE(ed.add_widget(ui::kNoWidgetIndex, ui::WidgetKind::Stack));
    const u32 stack = ed.selected();
    CHECK(path_of(ed, stack) == "stack");
    REQUIRE(ed.add_widget(stack, ui::WidgetKind::Text));
    REQUIRE(ed.add_widget(stack, ui::WidgetKind::Button));
    REQUIRE(ed.add_widget(stack, ui::WidgetKind::Button));
    CHECK(path_of(ed, ed.selected()) == "stack/button2");
    REQUIRE(ed.data().widgets.size() == 4);
    CHECK(ed.data().widgets[ed.selected()].styles[0].tint ==
          ui::default_theme().styles[u32(ui::WidgetKind::Button)][0].tint);
    REQUIRE(ed.add_widget(ui::kNoWidgetIndex, ui::WidgetKind::Panel));
    CHECK(path_of(ed, ed.selected()) == "panel");  // after the stack's subtree
    CHECK(ed.selected() == 4);

    // Fields, nav and focus.
    ui::DocumentData next = ed.data();
    const u32 play = next.find("stack/button"), quit = next.find("stack/button2");
    next.widgets[play].text.assign("PLAY");
    next.widgets[play].action.assign("Play");
    next.widgets[play].nav[ui::NavDown] = quit;
    next.widgets[quit].nav[ui::NavUp] = play;
    next.default_focus = play;
    REQUIRE(ed.apply(next));
    CHECK(ed.dirty());

    // A bad edit is refused with the cook's message and changes nothing.
    const u64 rev = ed.revision();
    next = ed.data();
    next.widgets[play].format.assign("%s");
    CHECK_FALSE(ed.apply(next));
    CHECK(std::string(ed.last_error().data(), ed.last_error().size()).find("format") != std::string::npos);
    next = ed.data();
    next.widgets[quit].name.assign("button");  // a sibling has it
    CHECK_FALSE(ed.apply(next));
    CHECK(ed.revision() == rev);
    CHECK(ed.data().widgets[play].format.empty());

    // Saving writes the canonical text, which the cook reads to the same document.
    REQUIRE(ed.save(session).has_value());
    CHECK_FALSE(ed.dirty());
    const std::string saved = file_text(file);
    CHECK(saved == std::string(ed.text().view().data(), ed.text().view().size()));
    {
        const ui::DocumentData back = read_doc(saved);
        cook::String again;
        cook::write_ui_document(back, ed.guid(), again);
        CHECK(std::string(again.view().data(), again.view().size()) == saved);
    }
    CHECK(session.cook().ok());

    // Moving the quit button up keeps its nav edges on the same widgets.
    REQUIRE(ed.move_widget(quit, -1));
    CHECK(path_of(ed, ed.selected()) == "stack/button2");
    {
        const ui::DocumentData& d = ed.data();
        const u32 p = d.find("stack/button"), q = d.find("stack/button2");
        CHECK(q < p);
        CHECK(d.widgets[p].nav[ui::NavDown] == q);
        CHECK(d.widgets[q].nav[ui::NavUp] == p);
        CHECK(d.default_focus == p);
    }
    CHECK_FALSE(ed.move_widget(ed.data().find("stack"), -1));  // first among the roots
    REQUIRE(ed.save(session).has_value());

    // Removing the play button clears the edge and the focus that named it.
    REQUIRE(ed.remove_widget(ed.data().find("stack/button")));
    CHECK(ed.data().find("stack/button") == ui::kNoWidgetIndex);
    CHECK(ed.data().default_focus == ui::kNoWidgetIndex);
    CHECK(ed.data().widgets[ed.data().find("stack/button2")].nav[ui::NavUp] == ui::kNoWidgetIndex);
    REQUIRE(ed.save(session).has_value());
    const std::string removed = file_text(file);

    // Undo puts it back (on disk too), redo takes it away again.
    REQUIRE(ed.undo(session));
    CHECK(ed.data().find("stack/button") != ui::kNoWidgetIndex);
    CHECK(read_doc(file_text(file)).find("stack/button") != ui::kNoWidgetIndex);
    REQUIRE(ed.redo(session));
    CHECK(file_text(file) == removed);
    REQUIRE(ed.undo(session));
    REQUIRE(ed.undo(session));  // before the move
    CHECK(ed.data().find("stack/button") < ed.data().find("stack/button2"));

    // A change on disk (another tool) is read in while nothing is unsaved here.
    std::string other = file_text(file);
    const usize at = other.find("\"PLAY\"");
    REQUIRE(at != std::string::npos);
    other.replace(at, 6, "\"START\"");
    REQUIRE(cook::fs::write_file_atomic(sv(file),
                                        Span<const byte>(reinterpret_cast<const byte*>(other.data()), other.size()))
                .has_value());
    CHECK(ed.sync(session));
    CHECK(ed.data().widgets[ed.data().find("stack/button")].text.view() == "START");
    CHECK_FALSE(ed.sync(session));

    // The kind changes the look.
    REQUIRE(ed.set_kind(ed.data().find("panel"), ui::WidgetKind::Button));
    CHECK(ed.data().widgets[ed.data().find("panel")].styles[1].tint ==
          ui::default_theme().styles[u32(ui::WidgetKind::Button)][1].tint);
}

TEST_CASE("UI document editor: the inspector edits a widget and the document through DrawCtx") {
    const std::string root = ui_project("ui_document_inspector");
    EditorSession session;
    REQUIRE(session.open_project(root).has_value());
    UiDocumentEditor ed;
    REQUIRE(ed.create(session, "ui/hud.mcui", false).has_value());
    REQUIRE(ed.add_widget(ui::kNoWidgetIndex, ui::WidgetKind::Button));
    const u32 b = ed.selected();
    const ui::Style before = ed.data().widgets[b].styles[1];

    ScriptCtx ctx;
    ctx.edits["Widget/Text"] = [](void* v) { *static_cast<std::string*>(v) = "GO"; };
    ctx.edits["Widget/Action"] = [](void* v) { *static_cast<std::string*>(v) = "Start"; };
    ctx.edits["Style/tint"] = [](void* v) {
        f32* c = static_cast<f32*>(v);
        c[0] = 1.0f, c[1] = 0.0f, c[2] = 0.0f, c[3] = 1.0f;
    };
    ctx.edits["Anchors/max"] = [](void* v) {
        f32* m = static_cast<f32*>(v);
        m[0] = 0.75f, m[1] = 0.75f;
    };
    u32 state = 0;
    bool every = true;
    REQUIRE(draw_ui_inspector(ctx, ed, state, every));
    const ui::WidgetData& w = ed.data().widgets[b];
    CHECK(w.text.view() == "GO");
    CHECK(w.action.view() == "Start");
    for (u32 s = 0; s < ui::kWidgetStateCount; ++s) CHECK(w.styles[s].tint.r == 1.0f);  // every state
    CHECK(w.styles[1].text_color == before.text_color);                                 // only the tint
    CHECK(w.anchors.max.x == 0.75f);
    CHECK(std::find(ctx.seen.begin(), ctx.seen.end(), "Nav/Down") != ctx.seen.end());

    // One state only.
    ScriptCtx one;
    one.edits["Style/tint"] = [](void* v) { static_cast<f32*>(v)[1] = 1.0f; };
    state = 1;
    every = false;
    REQUIRE(draw_ui_inspector(one, ed, state, every));
    CHECK(ed.data().widgets[b].styles[1].tint.g == 1.0f);
    CHECK(ed.data().widgets[b].styles[0].tint.g == 0.0f);

    // Nothing selected: the document's own fields.
    ed.select(ui::kNoWidgetIndex);
    ScriptCtx doc;
    doc.edits["Document/Modal"] = [](void* v) { *static_cast<bool*>(v) = true; };
    REQUIRE(draw_ui_inspector(doc, ed, state, every));
    CHECK(ed.data().modal);
    CHECK(std::find(doc.seen.begin(), doc.seen.end(), "Document/Theme") != doc.seen.end());
    REQUIRE(ed.save(session).has_value());
    CHECK(session.cook().ok());
}

TEST_CASE("UI document editor: subtree_end and is_inside follow the widget tree") {
    const std::string root = ui_project("ui_document_tree");
    EditorSession session;
    REQUIRE(session.open_project(root).has_value());
    UiDocumentEditor ed;
    REQUIRE(ed.create(session, "ui/tree.mcui").has_value());
    // stack(0) { panel(1) { button(2), button(3) }, text(4) }
    REQUIRE(ed.add_widget(ui::kNoWidgetIndex, ui::WidgetKind::Stack));
    REQUIRE(ed.add_widget(0, ui::WidgetKind::Panel));
    REQUIRE(ed.add_widget(1, ui::WidgetKind::Button));
    REQUIRE(ed.add_widget(1, ui::WidgetKind::Button));
    REQUIRE(ed.add_widget(0, ui::WidgetKind::Text));
    REQUIRE(ed.data().widgets.size() == 5);

    CHECK(ed.subtree_end(0) == 5);
    CHECK(ed.subtree_end(1) == 4);
    CHECK(ed.subtree_end(2) == 3);
    CHECK(ed.subtree_end(3) == 4);
    CHECK(ed.subtree_end(4) == 5);
    CHECK(ed.subtree_end(9) == 5);  // past the end: the end

    CHECK(ed.is_inside(2, 1));
    CHECK(ed.is_inside(3, 0));
    CHECK(ed.is_inside(1, 1));  // a widget is inside itself
    CHECK_FALSE(ed.is_inside(1, 2));
    CHECK_FALSE(ed.is_inside(4, 1));
    CHECK_FALSE(ed.is_inside(2, 3));  // siblings
    CHECK_FALSE(ed.is_inside(9, 0));  // out of range
}
