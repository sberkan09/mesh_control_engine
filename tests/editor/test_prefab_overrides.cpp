// Prefab overrides (FR-EDT-080), headless: an instance overrides the prefab root field by field,
// the inspector marks what is overridden, revert drops overrides, apply writes them into the
// prefab asset (every instance follows), and a prefab asset opens, edits and saves like a scene.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "cook_test_util.h"
#include "mce/editor/editor_session.h"
#include "mce/editor/inspector.h"
#include "mce/kit/components.h"

using namespace mce;
using namespace mce::editor;

namespace {

constexpr const char* kStack = "5ce4e000-0000-4000-8000-0000000000f1";
constexpr const char* kCrate = "5ce4e000-0000-4000-8000-000000000001";

void put(const std::string& path, const std::string& text) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

std::string get(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The root (Bottom) draws at order 3, size 2x2; Top hangs off it.
std::string stack_prefab() {
    return std::string(R"({ "format": "mce-prefab", "version": 1, "guid": ")") + kStack + R"(", "name": "stack",
  "entities": [
    { "name": "Bottom", "components": {
        "LocalTransform": { "position": [0, 0, 0] },
        "Sprite": { "texture": "guid:)" + kCrate + R"(", "size": [2, 2], "order": 3 } } },
    { "name": "Top", "components": {
        "LocalTransform": { "position": [0, 1, 0] },
        "ChildOf": { "parent": "$0" },
        "Sprite": { "texture": "guid:)" + kCrate + R"(" } } }
  ] })";
}

// A inherits everything; B overrides the order back to the C++ default (0).
std::string two_instances() {
    return std::string(R"({ "format": "mce-scene", "version": 1, "guid": "5ce4e000-0000-4000-8000-0000000000f3",
  "name": "two", "entities": [
    { "name": "A", "prefab": "guid:)") + kStack + R"(", "components": {
        "LocalTransform": { "position": [3, 0, 0] } } },
    { "name": "B", "prefab": "guid:)" + kStack + R"(", "components": {
        "LocalTransform": { "position": [-3, 0, 0] },
        "Sprite": { "order": 0 } } }
  ] })";
}

bool close(f32 a, f32 b) { return std::fabs(a - b) <= 1e-4f; }

// The Sprite of the ghost standing in for an instance's prefab root (at the instance's x, y 0).
const kit::Sprite* ghost_root(EditorScene& s, f32 x) {
    const kit::Sprite* found = nullptr;
    s.world().query<const WorldTransform, const kit::Sprite>().each([&](Entity e, const WorldTransform& t, const kit::Sprite& sp) {
        if (s.entity_for(e) == kNoEntity && close(t.position.x, x) && close(t.position.y, 0.0f)) found = &sp;
    });
    return found;
}

EntityId by_name(const EditorScene& s, StringView name) {
    for (const DocEntity& e : s.doc().entities())
        if (e.name.view() == name) return e.id;
    return kNoEntity;
}

std::string line_of(const cook::String& text, const std::string& entity) {
    const std::string t(text.view());
    const usize at = t.find("\"name\": \"" + entity + "\"");
    const usize sprite = t.find("\"Sprite\"", at);
    if (at == std::string::npos || sprite == std::string::npos) return {};
    return t.substr(sprite, t.find('\n', sprite) - sprite);
}

std::string project(const char* name) {
    const std::string root = cook_test::fresh_dir(name);
    std::filesystem::copy(std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets", root + "/assets",
                          std::filesystem::copy_options::recursive);
    put(root + "/assets/prefabs/stack.mcprefab", stack_prefab());
    put(root + "/assets/scenes/two.mcscene", two_instances());
    return root;
}

// Records the override marks per "Component.field" and answers one scripted action.
class MarkCtx final : public DrawCtx {
public:
    std::vector<std::string> overridden, inherited, read_only_rows;
    std::string act_on;
    OverrideAction action = OverrideAction::None;

    bool begin_component(const char* type_name, bool*) override {
        comp_ = type_name;
        return true;
    }
    void end_component() override { comp_.clear(); }
    bool begin_group(const char*, bool) override { return false; }
    void end_group() override {}
    bool text_field(const char* l, char*, usize) override { return field(l); }
    bool checkbox(const char* l, bool*) override { return field(l); }
    bool int_field(const char* l, i64*, i64, i64) override { return field(l); }
    bool float_field(const char* l, f64*, f64, f64) override { return field(l); }
    bool float_n(const char* l, f32*, u32, f32, f32) override { return field(l); }
    bool color_field(const char* l, f32*) override { return field(l); }
    bool enum_field(const char* l, i64*, const refl::EnumRecord&) override { return field(l); }
    bool asset_field(const char* l, asset::Guid*, StringView) override { return field(l); }
    bool entity_field(const char* l, EntityId*) override { return field(l); }
    void read_only(const char* l, StringView text) override { read_only_rows.push_back(std::string(l) + "=" + std::string(text)); }
    void help(const char*) override {}
    const refl::TypeRecord* add_component_menu(Span<const refl::TypeRecord* const>) override { return nullptr; }
    void override_mark(bool o) override { mark_ = o ? 1 : 0; }
    OverrideAction override_actions(bool, bool) override { return comp_ == act_on ? action : OverrideAction::None; }

private:
    bool field(const char* label) {
        if (mark_ >= 0 && !comp_.empty()) (mark_ ? overridden : inherited).push_back(comp_ + "." + label);
        mark_ = -1;
        return false;
    }
    std::string comp_;
    int mark_ = -1;
};

bool has(const std::vector<std::string>& v, const std::string& s) {
    for (const std::string& x : v)
        if (x == s) return true;
    return false;
}

void bump_and_reload(EditorSession& session, const std::string& path) {
    const auto mt = std::filesystem::last_write_time(path);
    std::filesystem::last_write_time(path, mt + std::chrono::seconds(2));
    session.tick_hot_reload(0.0, 0.0);
    session.tick_hot_reload(1.0, 0.0);
}

}  // namespace

TEST_CASE("prefab overrides: field by field; an override back to the default survives a save") {
    const std::string root = project("prefab_overrides");
    EditorSession session;
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    REQUIRE(session.open_scene("scenes/two.mcscene"));
    EditorScene& s = session.scene();
    const EntityId a = by_name(s, "A");
    const EntityId b = by_name(s, "B");
    REQUIRE(s.ghost_count() == 4);
    CHECK(s.verify_world());

    // A carries no Sprite of its own: the root's is inherited as it is.
    const kit::Sprite* ga = ghost_root(s, 3.0f);
    REQUIRE(ga);
    CHECK(ga->order == 3);
    CHECK(close(ga->size.x, 2.0f));
    REQUIRE(s.prefab_base(a, refl::type_of<kit::Sprite>()));
    CHECK(static_cast<const kit::Sprite*>(s.prefab_base(a, refl::type_of<kit::Sprite>())->data())->order == 3);

    // B overrides only the order (to 0, the C++ default); its size follows the prefab, in the
    // document too (the inspector shows effective values).
    const kit::Sprite* gb = ghost_root(s, -3.0f);
    REQUIRE(gb);
    CHECK(gb->order == 0);
    CHECK(close(gb->size.x, 2.0f));
    REQUIRE(s.get<kit::Sprite>(b));
    CHECK(close(s.get<kit::Sprite>(b)->size.x, 2.0f));
    cook::String text;
    text.clear();
    s.serialize(text);
    CHECK(line_of(text, "B") == R"("Sprite": { "order": 0 })");

    // The inspector marks the override and lists the inherited component on A.
    MarkCtx ui;
    draw_inspector(ui, s, b);
    CHECK(has(ui.overridden, "Sprite.order"));
    CHECK(has(ui.inherited, "Sprite.size"));
    MarkCtx ui_a;
    draw_inspector(ui_a, s, a);
    CHECK(has(ui_a.read_only_rows, "Inherited=Sprite"));

    // Editing a field makes it an override; undo takes it back.
    kit::Sprite sp = *s.get<kit::Sprite>(b);
    sp.size = {5.0f, 5.0f};
    REQUIRE(s.set(b, sp));
    text.clear();
    s.serialize(text);
    CHECK(line_of(text, "B") == R"("Sprite": { "size": [5, 5], "order": 0 })");
    CHECK(close(ghost_root(s, -3.0f)->size.x, 5.0f));
    REQUIRE(s.undo());
    text.clear();
    s.serialize(text);
    CHECK(line_of(text, "B") == R"("Sprite": { "order": 0 })");
    CHECK(close(ghost_root(s, -3.0f)->size.x, 2.0f));
    CHECK(s.verify_world());

    // Save + reopen keeps exactly the override.
    REQUIRE(session.save_scene());
    REQUIRE(session.open_scene("scenes/two.mcscene"));
    CHECK(ghost_root(session.scene(), -3.0f)->order == 0);
    CHECK(get(root + "/assets/scenes/two.mcscene").find(R"("Sprite": { "order": 0 })") != std::string::npos);
}

TEST_CASE("prefab overrides: revert drops them, apply writes them into the prefab for every instance") {
    const std::string root = project("prefab_apply");
    EditorSession session;
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    REQUIRE(session.open_scene("scenes/two.mcscene"));
    EditorScene& s = session.scene();
    const EntityId a = by_name(s, "A");
    const EntityId b = by_name(s, "B");

    // Revert through the inspector: B's Sprite is the prefab's again (and leaves the document).
    MarkCtx ui;
    ui.act_on = "Sprite";
    ui.action = DrawCtx::OverrideAction::Revert;
    CHECK(draw_inspector(ui, s, b));
    CHECK_FALSE(s.get<kit::Sprite>(b));
    CHECK(ghost_root(s, -3.0f)->order == 3);
    REQUIRE(s.undo());
    CHECK(ghost_root(s, -3.0f)->order == 0);

    // The placement is never applied to the prefab.
    CHECK_FALSE(s.apply_override(b, refl::type_of<LocalTransform>()));

    // Apply B's order 0 + size 5: the prefab root takes them, A (no overrides) follows.
    kit::Sprite sp = *s.get<kit::Sprite>(b);
    sp.size = {5.0f, 5.0f};
    REQUIRE(s.set(b, sp));
    REQUIRE(s.apply_override(b, refl::type_of<kit::Sprite>()));
    const std::string prefab = get(root + "/assets/prefabs/stack.mcprefab");
    CHECK(prefab.find(R"("format": "mce-prefab")") != std::string::npos);
    CHECK(prefab.find(R"("size": [5, 5])") != std::string::npos);
    CHECK_FALSE(s.get<kit::Sprite>(b));  // nothing of its own left
    const kit::Sprite* ga = ghost_root(s, 3.0f);
    REQUIRE(ga);
    CHECK(close(ga->size.x, 5.0f));
    CHECK(ga->order == 0);
    CHECK(close(ghost_root(s, -3.0f)->size.x, 5.0f));
    CHECK(s.verify_world());
    (void)a;
}

TEST_CASE("prefab overrides: a prefab asset opens, edits and saves; instances follow on reload") {
    const std::string root = project("prefab_open");
    EditorSession session;
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    REQUIRE(session.open_scene("prefabs/stack.mcprefab"));
    EditorScene& p = session.scene();
    CHECK(p.doc().kind() == DocKind::Prefab);
    REQUIRE(p.doc().size() == 2);
    const EntityId bottom = p.doc().at(0).id;
    kit::Sprite sp = *p.get<kit::Sprite>(bottom);
    sp.order = 7;
    REQUIRE(p.set(bottom, sp));
    REQUIRE(session.save_scene());
    const std::string text = get(root + "/assets/prefabs/stack.mcprefab");
    CHECK(text.find(R"("format": "mce-prefab")") != std::string::npos);
    CHECK(text.find(R"("order": 7)") != std::string::npos);
    CHECK(text.find("settings") == std::string::npos);

    bump_and_reload(session, root + "/assets/prefabs/stack.mcprefab");
    REQUIRE(session.open_scene("scenes/two.mcscene"));
    EditorScene& s = session.scene();
    CHECK(s.doc().kind() == DocKind::Scene);
    CHECK(ghost_root(s, 3.0f)->order == 7);   // A inherits the edit
    CHECK(ghost_root(s, -3.0f)->order == 0);  // B keeps its override
}
