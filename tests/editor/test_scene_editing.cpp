// editor_core scene authoring, headless (editor-tools.md §2.1): SceneDocument round-trip, the
// mutation funnel + preview-world mirror, the reflection inspector through a scripted DrawCtx,
// viewport math/picking, and the project session (open -> edit -> save -> cook).

#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "cook_test_util.h"
#include "mce/ecs/transform.h"
#include "mce/editor/editor_session.h"
#include "mce/editor/inspector.h"
#include "mce/editor/viewport2d.h"
#include "mce/kit/components.h"
#include "mce/physics/components.h"

using namespace mce;
using namespace mce::editor;
using namespace cook_test;

namespace edtest {

struct EdFollow {
    Entity target;
    f32 distance = 2.0f;
    u32 pad_ = 0;
};
MCE_COMPONENT(EdFollow, target, distance)

}  // namespace edtest

namespace {

const std::string kDemoAssets = std::string(MCE_SOURCE_DIR) + "/samples/scene_demo/assets";
const std::string kDemoScene = kDemoAssets + "/scenes/demo.mcscene";

std::string serialize(const SceneDocument& d) {
    cook::String s;
    d.serialize(s);
    return std::string(s.view().data(), s.view().size());
}

std::string diag_text(const cook::DiagList& diags) {
    std::string s;
    for (const cook::Diagnostic& d : diags.all()) {
        cook::String line;
        cook::format_diagnostic(d, line);
        s += std::string(line.view().data(), line.view().size()) + "\n";
    }
    return s;
}

// Copies the scene_demo assets into a fresh project folder.
std::string demo_project(const char* name) {
    const std::string root = fresh_dir(name);
    std::filesystem::copy(kDemoAssets, root + "/assets", std::filesystem::copy_options::recursive);
    return root;
}

// A DrawCtx that answers from a script: edits keyed "Component.group.label" (or "Name").
class ScriptCtx final : public DrawCtx {
public:
    using Edit = std::function<bool(void*)>;
    std::map<std::string, Edit> edits;
    std::vector<std::string> seen;
    std::string remove;                     // component to remove
    const refl::TypeRecord* add = nullptr;  // component to add
    std::vector<std::string> addable;

    bool begin_component(const char* type_name, bool* rm) override {
        comp_ = type_name;
        groups_.clear();
        if (remove == type_name) *rm = true;
        return true;
    }
    void end_component() override { comp_.clear(); }
    bool begin_group(const char* label, bool) override {
        groups_.push_back(label);
        return true;
    }
    void end_group() override { groups_.pop_back(); }

    bool text_field(const char* l, char* buf, usize cap) override {
        return apply_with(l, [&](const Edit& e) {
            std::string v;
            e(&v);
            std::snprintf(buf, cap, "%s", v.c_str());
            return true;
        });
    }
    bool checkbox(const char* l, bool* v) override { return apply(l, v); }
    bool int_field(const char* l, i64* v, i64, i64) override { return apply(l, v); }
    bool float_field(const char* l, f64* v, f64, f64) override { return apply(l, v); }
    bool float_n(const char* l, f32* v, u32, f32, f32) override { return apply(l, v); }
    bool color_field(const char* l, f32* rgba) override { return apply(l, rgba); }
    bool enum_field(const char* l, i64* v, const refl::EnumRecord&) override { return apply(l, v); }
    bool asset_field(const char* l, asset::Guid* g, StringView type) override {
        seen.push_back(key(l) + ":" + std::string(type.data(), type.size()));
        return apply(l, g, false);
    }
    bool entity_field(const char* l, EntityId* id) override { return apply(l, id); }
    void read_only(const char* l, StringView) override { seen.push_back(key(l) + " (ro)"); }
    void help(const char*) override {}
    const refl::TypeRecord* add_component_menu(Span<const refl::TypeRecord* const> list) override {
        addable.clear();
        for (const refl::TypeRecord* t : list) addable.emplace_back(t->name);
        return add;
    }

private:
    std::string key(const char* label) const {
        std::string k = comp_.empty() ? std::string() : comp_ + ".";
        for (const std::string& g : groups_) k += g + ".";
        return k + label;
    }
    template <class F>
    bool apply_with(const char* label, F&& with) {
        const std::string k = key(label);
        seen.push_back(k);
        auto it = edits.find(k);
        return it != edits.end() && with(it->second);
    }
    bool apply(const char* label, void* p, bool record = true) {
        const std::string k = key(label);
        if (record) seen.push_back(k);
        auto it = edits.find(k);
        return it != edits.end() && it->second(p);
    }

    std::string comp_;
    std::vector<std::string> groups_;
};

bool contains(const std::vector<std::string>& v, const std::string& s) {
    for (const std::string& x : v)
        if (x == s) return true;
    return false;
}

}  // namespace

// ---- SceneDocument --------------------------------------------------------------------------------

TEST_CASE("SceneDocument: parse -> serialize -> parse is byte-stable") {
    const std::string text = read_file(kDemoScene);
    REQUIRE_FALSE(text.empty());
    SceneDocument a;
    cook::DiagList diags;
    REQUIRE_MESSAGE(a.parse(StringView(text.data(), text.size()), "demo.mcscene", diags), diag_text(diags));
    CHECK(a.size() == 6);
    CHECK(a.at(5).name.view() == "Title");
    CHECK(asset::to_string(a.guid()).view() == "5ce4e000-0000-4000-8000-000000000100");

    const std::string once = serialize(a);
    SceneDocument b;
    REQUIRE(b.parse(StringView(once.data(), once.size()), "once", diags));
    CHECK(serialize(b) == once);
    // The canonical form is what the editor writes, and the sample is saved in it
    // (MCE_UPDATE_SAMPLES=1 rewrites it after a deliberate format change).
    if (std::getenv("MCE_UPDATE_SAMPLES")) write_file(kDemoScene, once);
    CHECK(once == text);
}

TEST_CASE("SceneDocument: newer format versions and unknown components are file:line errors") {
    SceneDocument d;
    cook::DiagList diags;
    const char* future = R"({ "format": "mce-scene", "version": 99, "guid": "5ce4e000-2222-4000-8000-000000000001",
        "name": "f", "entities": [] })";
    CHECK_FALSE(d.parse(future, "future.mcscene", diags));
    CHECK(diag_text(diags).find("future.mcscene:1") != std::string::npos);

    cook::DiagList d2;
    const char* unknown = "{ \"format\": \"mce-scene\", \"version\": 1, \"guid\": \"5ce4e000-2222-4000-8000-000000000001\",\n"
                          "  \"name\": \"u\", \"entities\": [\n"
                          "    { \"name\": \"A\", \"components\": { \"NoSuchThing\": {} } } ] }";
    CHECK_FALSE(d.parse(unknown, "u.mcscene", d2));
    const std::string msg = diag_text(d2);
    CHECK(msg.find("u.mcscene:3:") != std::string::npos);
    CHECK(msg.find("NoSuchThing") != std::string::npos);
}

TEST_CASE("SceneDocument: prefab instance links and their overrides survive a round-trip") {
    const char* text = "{\n  \"format\": \"mce-scene\",\n  \"version\": 1,\n"
                       "  \"guid\": \"5ce4e000-2222-4000-8000-000000000002\",\n  \"name\": \"p\",\n  \"entities\": [\n"
                       "    {\n      \"name\": \"Grunt\",\n      \"prefab\": \"guid:5ce4e000-2222-4000-8000-000000000003\",\n"
                       "      \"components\": {\n        \"LocalTransform\": { \"position\": [2, 0, 0] }\n      }\n    }\n  ]\n}\n";
    SceneDocument d;
    cook::DiagList diags;
    REQUIRE_MESSAGE(d.parse(text, "p.mcscene", diags), diag_text(diags));
    CHECK(asset::to_string(d.at(0).prefab).view() == "5ce4e000-2222-4000-8000-000000000003");
    CHECK(serialize(d) == text);
}

// ---- EditorScene: funnel + preview world ---------------------------------------------------------

TEST_CASE("EditorScene: commands keep the preview world in sync with the document") {
    EditorScene s;
    cook::DiagList diags;
    REQUIRE(s.open(StringView(kDemoScene.data(), kDemoScene.size()), diags));
    CHECK_FALSE(s.dirty());
    CHECK(s.world().entity_count() == 6);
    CHECK(s.verify_world());

    const EntityId a = s.create_entity("Spawned");
    CHECK(s.dirty());
    CHECK(s.get<LocalTransform>(a));
    CHECK(s.world().entity_count() == 7);
    LocalTransform t;
    t.position = {5, 6, 0};
    CHECK(s.set(a, t));
    CHECK_FALSE(s.set(a, t));  // unchanged: no-op
    const LocalTransform* rt = s.world().try_get<LocalTransform>(s.runtime_entity(a));
    REQUIRE(rt);
    CHECK(rt->position.x == 5.0f);
    CHECK(s.verify_world());

    // A random command soup never lets the two worlds drift.
    std::mt19937 gen(1234);
    auto rng = [&] { return static_cast<u32>(gen()); };
    std::vector<EntityId> ids;
    for (usize i = 0; i < s.doc().size(); ++i) ids.push_back(s.doc().at(i).id);
    for (int step = 0; step < 400; ++step) {
        const u32 op = rng() % 6;
        const EntityId pick = ids.empty() ? kNoEntity : ids[rng() % ids.size()];
        switch (op) {
            case 0: ids.push_back(s.create_entity("e", rng() % 2 == 0)); break;
            case 1:
                if (pick && s.destroy_entity(pick)) ids.erase(std::find(ids.begin(), ids.end(), pick));
                break;
            case 2: s.add_component(pick, refl::type_of<kit::Sprite>()); break;
            case 3: s.remove_component(pick, refl::type_of<kit::Sprite>()); break;
            case 4: {
                LocalTransform v;
                v.position = {static_cast<f32>(rng() % 100), 1, 0};
                s.add_component(pick, refl::type_of<LocalTransform>());
                s.set(pick, v);
                break;
            }
            case 5: s.move_entity(pick, rng() % (s.doc().size() + 1)); break;
            default: break;
        }
        REQUIRE_MESSAGE(s.verify_world(), "drift after step ", step, " op ", op);
    }
    CHECK(s.world().entity_count() == s.doc().size());
}

TEST_CASE("EditorScene: destroying an entity nulls references to it in document and world") {
    EditorScene s;
    const EntityId target = s.create_entity("Target");
    const EntityId user = s.create_entity("User");
    s.add_component(user, refl::type_of<edtest::EdFollow>());
    edtest::EdFollow f;
    f.target = doc_entity_ref(target);
    CHECK(s.set(user, f));
    const edtest::EdFollow* live = s.world().try_get<edtest::EdFollow>(s.runtime_entity(user));
    REQUIRE(live);
    CHECK(live->target == s.runtime_entity(target));
    CHECK(s.verify_world());

    CHECK(s.destroy_entity(target));
    CHECK(s.get<edtest::EdFollow>(user)->target.is_null());
    CHECK(s.world().try_get<edtest::EdFollow>(s.runtime_entity(user))->target.is_null());
    CHECK(s.verify_world());

    // The reference survives a save/load round-trip as a file index.
    const EntityId t2 = s.create_entity("Target2", true, 0);
    f.target = doc_entity_ref(t2);
    s.set(user, f);
    const std::string text = serialize(s.doc());
    CHECK(text.find("\"target\": \"$0\"") != std::string::npos);
    SceneDocument back;
    cook::DiagList diags;
    REQUIRE(back.parse(StringView(text.data(), text.size()), "t", diags));
    CHECK(serialize(back) == text);
}

// ---- inspector ----------------------------------------------------------------------------------

TEST_CASE("inspector: reflection widgets round-trip edits through the funnel (zero per-type code)") {
    EditorScene s;
    const EntityId id = s.create_entity("Thing");
    s.add_component(id, refl::type_of<kit::Sprite>());
    s.add_component(id, refl::type_of<kit::TextLabel>());
    s.add_component(id, refl::type_of<physics::CharacterGround2D>());

    // A pass with no edits: nothing changes, every authored field is visited, Transient is not.
    const u64 rev0 = s.revision();
    ScriptCtx look;
    CHECK_FALSE(draw_inspector(look, s, id));
    CHECK(s.revision() == rev0);
    CHECK(contains(look.seen, "Name"));
    CHECK(contains(look.seen, "LocalTransform.position"));
    CHECK(contains(look.seen, "LocalTransform.rotation"));
    CHECK(contains(look.seen, "Sprite.texture:Texture"));
    CHECK(contains(look.seen, "TextLabel.font:Font"));
    CHECK(contains(look.seen, "TextLabel.align"));
    CHECK(contains(look.seen, "Sprite.flip_x"));
    for (const std::string& k : look.seen) CHECK_MESSAGE(k.find("ground_body") == std::string::npos, k);
    CHECK_FALSE(contains(look.addable, "Sprite"));
    CHECK(contains(look.addable, "RigidBody2D"));

    const u64 rev = s.revision();
    const auto parsed = asset::parse_guid("5ce4e000-0000-4000-8000-000000000001");
    REQUIRE(parsed);
    const asset::Guid tex = parsed.value();
    ScriptCtx edit;
    edit.edits["Name"] = [](void* v) { *static_cast<std::string*>(v) = "Renamed"; return true; };
    edit.edits["LocalTransform.position"] = [](void* v) {
        f32* p = static_cast<f32*>(v);
        p[0] = 3; p[1] = 4; p[2] = 0;
        return true;
    };
    edit.edits["LocalTransform.rotation"] = [](void* v) { static_cast<f32*>(v)[2] = 90.0f; return true; };
    edit.edits["Sprite.flip_x"] = [](void* v) { *static_cast<bool*>(v) = true; return true; };
    edit.edits["Sprite.order"] = [](void* v) { *static_cast<i64*>(v) = 7; return true; };
    edit.edits["Sprite.tint"] = [](void* v) { static_cast<f32*>(v)[1] = 0.25f; return true; };
    edit.edits["Sprite.texture"] = [tex](void* v) { *static_cast<asset::Guid*>(v) = tex; return true; };
    edit.edits["TextLabel.text"] = [](void* v) { *static_cast<std::string*>(v) = "HELLO"; return true; };
    edit.edits["TextLabel.height"] = [](void* v) { *static_cast<f64*>(v) = 1.5; return true; };
    edit.edits["TextLabel.align"] = [](void* v) { *static_cast<i64*>(v) = static_cast<i64>(kit::TextAlign::Center); return true; };
    edit.add = &refl::type_of<physics::RigidBody2D>();
    CHECK(draw_inspector(edit, s, id));
    CHECK(s.revision() > rev);

    const DocEntity* e = s.doc().find(id);
    CHECK(e->name.view() == "Renamed");
    const LocalTransform* t = s.get<LocalTransform>(id);
    CHECK(t->position.x == 3.0f);
    CHECK(t->position.y == 4.0f);
    CHECK(quat_to_euler_deg(t->rotation).z == doctest::Approx(90.0f).epsilon(1e-4));
    const kit::Sprite* sp = s.get<kit::Sprite>(id);
    CHECK(sp->flip_x);
    CHECK(sp->order == 7);
    CHECK(sp->tint.g == 0.25f);
    CHECK(sp->texture.guid == tex);
    const kit::TextLabel* tl = s.get<kit::TextLabel>(id);
    CHECK(tl->text.view() == "HELLO");
    CHECK(tl->height == 1.5f);
    CHECK(tl->align == kit::TextAlign::Center);
    CHECK(s.get<physics::RigidBody2D>(id));
    CHECK(s.verify_world());

    // Remove through the component header.
    ScriptCtx rm;
    rm.remove = "TextLabel";
    CHECK(draw_inspector(rm, s, id));
    CHECK_FALSE(s.get<kit::TextLabel>(id));
    CHECK(s.verify_world());

    // The edited values survive serialization.
    const std::string text = serialize(s.doc());
    CHECK(text.find("\"flip_x\": true") != std::string::npos);
    CHECK(text.find("\"order\": 7") != std::string::npos);
    CHECK(text.find("guid:5ce4e000-0000-4000-8000-000000000001") != std::string::npos);
    SceneDocument back;
    cook::DiagList diags;
    REQUIRE_MESSAGE(back.parse(StringView(text.data(), text.size()), "t", diags), diag_text(diags));
    CHECK(serialize(back) == text);
}

TEST_CASE("inspector: Euler <-> quaternion conversion round-trips") {
    for (f32 z : {0.0f, 45.0f, -90.0f, 179.0f}) {
        const math::vec3 e = quat_to_euler_deg(euler_deg_to_quat({10.0f, -20.0f, z}));
        CHECK(e.x == doctest::Approx(10.0f).epsilon(1e-3));
        CHECK(e.y == doctest::Approx(-20.0f).epsilon(1e-3));
        CHECK(e.z == doctest::Approx(z).epsilon(1e-3));
    }
}

// ---- viewport math ------------------------------------------------------------------------------

TEST_CASE("viewport: camera mapping, zoom-at-cursor and picking follow draw order") {
    EditorCamera2D cam;
    cam.center = {1, 2};
    const rhi::Extent2D size{800, 600};
    const math::vec2 mid = cam.world_to_screen({1, 2}, size);
    CHECK(mid.x == doctest::Approx(400.0f));
    CHECK(mid.y == doctest::Approx(300.0f));
    const math::vec2 w = cam.screen_to_world({448, 300 - 96}, size);
    CHECK(w.x == doctest::Approx(2.0f));
    CHECK(w.y == doctest::Approx(4.0f));
    const math::vec2 cursor{100, 50};
    const math::vec2 before = cam.screen_to_world(cursor, size);
    cam.zoom_at(cursor, size, 2.0f);
    const math::vec2 after = cam.screen_to_world(cursor, size);
    CHECK(after.x == doctest::Approx(before.x));
    CHECK(after.y == doctest::Approx(before.y));
    CHECK(cam.zoom == doctest::Approx(2.0f));

    EditorScene s;
    const EntityId back = s.create_entity("Back");
    s.add_component(back, refl::type_of<kit::Sprite>());
    kit::Sprite big;
    big.size = {4, 4};
    s.set(back, big);
    const EntityId front = s.create_entity("Front");
    s.add_component(front, refl::type_of<kit::Sprite>());
    kit::Sprite small;
    small.order = 5;
    s.set(front, small);
    const EntityId marker = s.create_entity("Marker");
    LocalTransform mt;
    mt.position = {10, 0, 0};
    s.set(marker, mt);

    CHECK(pick_entity(s, {0.2f, 0.2f}) == front);   // higher order wins even though it is
    CHECK(pick_entity(s, {1.5f, 1.5f}) == back);    // smaller
    CHECK(pick_entity(s, {10.05f, 0.0f}) == marker);
    CHECK(pick_entity(s, {50, 50}) == kNoEntity);

    // Rotation and scale move the quad.
    LocalTransform rt;
    rt.position = {0, 0, 0};
    rt.scale = {2, 1, 1};
    rt.rotation = euler_deg_to_quat({0, 0, 90});
    s.set(front, rt);
    math::vec2 q[4];
    REQUIRE(entity_quad(s, front, q));
    CHECK(point_in_quad(q, {0.0f, 0.9f}));
    CHECK_FALSE(point_in_quad(q, {0.9f, 0.0f}));

    const math::aabb2 b = scene_bounds(s);
    CHECK(b.min.x <= -2.0f);
    CHECK(b.max.x >= 10.0f);
}

// ---- session: project open -> edit -> save -> cook ---------------------------------------------

TEST_CASE("session: open the demo project, edit, save, cook; a broken scene is a file:line cook error") {
    const std::string root = demo_project("session_demo");
    EditorSession session;
    REQUIRE(session.open_project(StringView(root.data(), root.size())));
    REQUIRE(session.project().mounted());  // first open cooks
    CHECK(session.project().last_report().ok());

    Array<const ProjectAsset*> scenes;
    session.list_scenes(scenes);
    REQUIRE(scenes.size() == 1);
    CHECK(scenes[0]->path.view() == "scenes/demo.mcscene");
    const ProjectAsset* tex = nullptr;
    for (const ProjectAsset& a : session.project().assets())
        if (a.path.view() == "textures/crate.png") tex = &a;
    REQUIRE(tex);
    CHECK(StringView(asset_type_name(tex->type)) == "Texture");

    REQUIRE(session.open_scene("scenes/demo.mcscene"));
    EditorScene& s = session.scene();
    CHECK(s.doc().size() == 6);
    const EntityId added = s.create_entity("Crate C");
    s.add_component(added, refl::type_of<kit::Sprite>());
    kit::Sprite sp;
    sp.texture.guid = tex->guid;
    s.set(added, sp);
    REQUIRE(session.save_scene());
    CHECK_FALSE(s.dirty());

    const cook::CookReport& r = session.cook();
    CHECK_MESSAGE(r.ok(), diag_text(r.diags));
    CHECK(r.cooked >= 1);  // the scene re-cooked
    asset::AssetSystem* mounted = session.project().mounted();
    REQUIRE(mounted);
    auto scene = mounted->load_sync<asset::Scene>(s.doc().guid());
    REQUIRE(scene);

    // Reopen from disk: the edit is there.
    REQUIRE(session.open_scene("scenes/demo.mcscene"));
    CHECK(session.scene().doc().size() == 7);
    CHECK(session.scene().doc().at(6).name.view() == "Crate C");

    // Save As gets a new GUID (two files never share one), and the catalog sees the copy.
    const asset::Guid old = session.scene().doc().guid();
    REQUIRE(session.save_scene_as("scenes/copy.mcscene"));
    CHECK_FALSE(session.scene().doc().guid() == old);
    session.list_scenes(scenes);
    CHECK(scenes.size() == 2);

    // A broken scene: the cook reports file+line, the previous output keeps serving.
    write_file(root + "/assets/scenes/copy.mcscene",
               read_file(root + "/assets/scenes/copy.mcscene").replace(0, 1, "{\n \"bogus\": ,"));
    const cook::CookReport& bad = session.cook();
    CHECK_FALSE(bad.ok());
    const std::string msg = diag_text(bad.diags);
    CHECK(msg.find("copy.mcscene:2") != std::string::npos);
    CHECK(session.project().mounted());
    CHECK(session.project().mounted()->load_sync<asset::Scene>(old));

    // An unreadable scene leaves the open one in place.
    const usize before = session.scene().doc().size();
    CHECK_FALSE(session.open_scene("scenes/copy.mcscene"));
    CHECK(session.scene_diags().error_count() > 0);
    CHECK(session.scene().doc().size() == before);
}
