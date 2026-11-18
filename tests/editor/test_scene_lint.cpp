// M-SCENELINT: mistakes a scene document carries without failing to load.

#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "mce/ecs/hierarchy.h"
#include "mce/ecs/transform.h"
#include "mce/editor/editor_scene.h"
#include "mce/editor/scene_lint.h"
#include "mce/kit/anim3d.h"

using namespace mce;
using namespace mce::editor;

namespace {

bool has(const Array<LintFinding>& all, EntityId id, LintKind kind) {
    for (const LintFinding& f : all)
        if (f.id == id && f.kind == kind) return true;
    return false;
}

void set_transform(EditorScene& s, EntityId id, const LocalTransform& t) {
    s.add_component(id, refl::type_of<LocalTransform>());
    (void)s.set(id, t);  // false when the value is already there; the checks below read the document
}

}  // namespace

TEST_CASE("a tidy scene has no findings") {
    EditorScene s;
    const EntityId room = s.create_entity("Room");
    const EntityId wall = s.create_entity("Wall");
    s.set_parent(wall, room);
    set_transform(s, wall, LocalTransform{});
    CHECK(lint_scene(s.doc()).empty());
    CHECK(format_lint(lint_scene(s.doc()).span()).empty());
}

TEST_CASE("siblings that share a name are reported, the same name under different parents is not") {
    EditorScene s;
    const EntityId room_a = s.create_entity("A");
    const EntityId room_b = s.create_entity("B");
    const EntityId first = s.create_entity("Wall");
    const EntityId second = s.create_entity("Wall");
    const EntityId elsewhere = s.create_entity("Wall");
    s.set_parent(first, room_a);
    s.set_parent(second, room_a);
    s.set_parent(elsewhere, room_b);
    const Array<LintFinding> f = lint_scene(s.doc());
    CHECK(has(f, second, LintKind::DuplicateSiblingName));
    CHECK_FALSE(has(f, first, LintKind::DuplicateSiblingName));
    CHECK_FALSE(has(f, elsewhere, LintKind::DuplicateSiblingName));
    const auto report = format_lint(f.span());  // a view of the temporary would dangle
    const std::string_view text = report.c_str();
    CHECK(text.find("duplicate name among its siblings") != std::string_view::npos);
}

TEST_CASE("empty names, zero scale and a transform that is not a number") {
    EditorScene s;
    const EntityId unnamed = s.create_entity("");
    const EntityId flat = s.create_entity("Flat");
    const EntityId broken = s.create_entity("Broken");
    LocalTransform t;
    t.scale = {1.0f, 0.0f, 1.0f};
    set_transform(s, flat, t);
    LocalTransform nan_t;
    nan_t.position.x = std::numeric_limits<f32>::quiet_NaN();
    set_transform(s, broken, nan_t);
    const Array<LintFinding> f = lint_scene(s.doc());
    CHECK(has(f, unnamed, LintKind::EmptyName));
    CHECK(has(f, flat, LintKind::ZeroScale));
    CHECK(has(f, broken, LintKind::NonFiniteTransform));
    CHECK_FALSE(has(f, broken, LintKind::ZeroScale));
}

TEST_CASE("a parent the scene lacks is reported") {
    EditorScene s;
    const EntityId orphan = s.create_entity("Orphan");
    s.add_component(orphan, refl::type_of<ChildOf>());
    REQUIRE(s.set(orphan, ChildOf{doc_entity_ref(999)}));
    CHECK(has(lint_scene(s.doc()), orphan, LintKind::MissingParent));
}

TEST_CASE("a reference to an entity the scene lacks is reported; a null one and a present one are not") {
    EditorScene s;
    const EntityId hand = s.create_entity("Hand");
    const EntityId gun = s.create_entity("Gun");
    s.add_component(gun, refl::type_of<kit::AttachToBone>());
    CHECK_FALSE(has(lint_scene(s.doc()), gun, LintKind::DanglingRef));  // a null target

    kit::AttachToBone a;
    a.target = doc_entity_ref(hand);
    REQUIRE(s.set(gun, a));
    CHECK_FALSE(has(lint_scene(s.doc()), gun, LintKind::DanglingRef));

    a.target = doc_entity_ref(999);
    REQUIRE(s.set(gun, a));
    const Array<LintFinding> f = lint_scene(s.doc());
    CHECK(has(f, gun, LintKind::DanglingRef));
    CHECK_FALSE(has(f, gun, LintKind::MissingParent));  // a parent is reported as MissingParent, not twice
    CHECK(std::string_view(format_lint(f.span()).c_str()).find("Gun (id ") != std::string_view::npos);
}

TEST_CASE("a loop of parents is reported once, on its lowest id; a chain into the loop and a tidy tree are not") {
    EditorScene s;
    const EntityId a = s.create_entity("A");
    const EntityId b = s.create_entity("B");
    const EntityId c = s.create_entity("C");
    const EntityId tail = s.create_entity("Tail");  // hangs below the loop, not in it
    const EntityId solo = s.create_entity("Solo");
    for (EntityId e : {a, b, c, tail}) s.add_component(e, refl::type_of<ChildOf>());
    REQUIRE(s.set(a, ChildOf{doc_entity_ref(c)}));
    REQUIRE(s.set(b, ChildOf{doc_entity_ref(a)}));
    REQUIRE(s.set(c, ChildOf{doc_entity_ref(b)}));
    REQUIRE(s.set(tail, ChildOf{doc_entity_ref(c)}));
    const Array<LintFinding> f = lint_scene(s.doc());
    usize cycles = 0;
    for (const LintFinding& x : f) cycles += x.kind == LintKind::ParentCycle ? 1 : 0;
    CHECK(cycles == 1);
    CHECK(has(f, std::min({a, b, c}), LintKind::ParentCycle));
    CHECK_FALSE(has(f, tail, LintKind::ParentCycle));
    CHECK_FALSE(has(f, solo, LintKind::ParentCycle));
    CHECK(std::string_view(format_lint(f.span()).c_str()).find("parents loop") != std::string_view::npos);
}

TEST_CASE("fix_scene_lint breaks a parent loop with one change and a second run finds nothing") {
    EditorScene s;
    const EntityId a = s.create_entity("A");
    const EntityId b = s.create_entity("B");
    s.add_component(a, refl::type_of<ChildOf>());
    s.add_component(b, refl::type_of<ChildOf>());
    REQUIRE(s.set(a, ChildOf{doc_entity_ref(b)}));
    REQUIRE(s.set(b, ChildOf{doc_entity_ref(a)}));
    SceneDocument doc = s.doc();
    CHECK(fix_scene_lint(doc) == 1);
    CHECK(lint_scene(doc).empty());
    CHECK(fix_scene_lint(doc) == 0);
    // The lowest id became a root; the other keeps its parent.
    const EntityId low = std::min(a, b), high = std::max(a, b);
    CHECK(doc.find(low)->find(refl::type_of<ChildOf>()) == nullptr);
    CHECK(doc.find(high)->find(refl::type_of<ChildOf>()) != nullptr);
}

TEST_CASE("fix_scene_lint repairs every kind and leaves a clean scene") {
    EditorScene s;
    const EntityId room = s.create_entity("Room");
    const EntityId first = s.create_entity("Wall");
    const EntityId second = s.create_entity("Wall");
    const EntityId third = s.create_entity("Wall");
    const EntityId unnamed = s.create_entity("");
    const EntityId flat = s.create_entity("Flat");
    const EntityId broken = s.create_entity("Broken");
    const EntityId orphan = s.create_entity("Orphan");
    const EntityId gun = s.create_entity("Gun");
    s.set_parent(first, room);
    s.set_parent(second, room);
    s.set_parent(third, room);
    LocalTransform flat_t;
    flat_t.scale = {1.0f, 0.0f, 1.0f};
    set_transform(s, flat, flat_t);
    LocalTransform nan_t;
    nan_t.position.x = std::numeric_limits<f32>::quiet_NaN();
    nan_t.scale = {2.0f, 0.0f, 2.0f};
    set_transform(s, broken, nan_t);
    s.add_component(orphan, refl::type_of<ChildOf>());
    REQUIRE(s.set(orphan, ChildOf{doc_entity_ref(999)}));
    s.add_component(gun, refl::type_of<kit::AttachToBone>());
    kit::AttachToBone a;
    a.target = doc_entity_ref(998);
    REQUIRE(s.set(gun, a));
    REQUIRE_FALSE(lint_scene(s.doc()).empty());

    SceneDocument doc = s.doc();
    const usize fixed = fix_scene_lint(doc);
    CHECK(fixed >= 7);
    CHECK(lint_scene(doc).empty());
    CHECK(doc.find(first)->name.view() == "Wall");
    CHECK(doc.find(second)->name.view() == "Wall (2)");
    CHECK(doc.find(third)->name.view() == "Wall (3)");
    CHECK(doc.find(unnamed)->name.view().size() > 0);
    CHECK(doc.find(orphan)->find(refl::type_of<ChildOf>()) == nullptr);
    const auto* fixed_flat = static_cast<const LocalTransform*>(doc.find(flat)->find(refl::type_of<LocalTransform>())->data());
    CHECK(fixed_flat->scale.y == 1.0f);
    const auto* fixed_broken = static_cast<const LocalTransform*>(doc.find(broken)->find(refl::type_of<LocalTransform>())->data());
    CHECK(fixed_broken->position.x == 0.0f);
    CHECK(fixed_broken->scale.x == 2.0f);  // a finite axis is kept
    CHECK(fixed_broken->scale.y == 1.0f);
    const auto* fixed_gun = static_cast<const kit::AttachToBone*>(doc.find(gun)->find(refl::type_of<kit::AttachToBone>())->data());
    CHECK(fixed_gun->target.is_null());

    CHECK(fix_scene_lint(doc) == 0);  // idempotent
}

TEST_CASE("fix_scene_lint on a tidy scene changes nothing") {
    EditorScene s;
    s.create_entity("A");
    s.create_entity("B");
    SceneDocument doc = s.doc();
    cook::String before, after;
    doc.serialize(before);
    CHECK(fix_scene_lint(doc) == 0);
    doc.serialize(after);
    CHECK(before.view() == after.view());
}

// Found by the fuzz below (seed 10): a parent link that names nothing is dropped, and the entity then joins the roots,
// where it shares its name with two others. One repair run has to rename it too.
TEST_CASE("fix_scene_lint renames the duplicates a dropped parent link creates") {
    EditorScene s;
    s.create_entity("Wall");
    const EntityId second = s.create_entity("Wall");
    const EntityId third = s.create_entity("Wall");
    s.add_component(third, refl::type_of<ChildOf>());
    REQUIRE(
        s.set(third, ChildOf{doc_entity_ref(900)}));  // a parent the scene lacks: its own sibling group, then a root
    SceneDocument doc = s.doc();
    CHECK(fix_scene_lint(doc) > 0);
    CHECK(lint_scene(doc).empty());
    CHECK(doc.find(second)->name.view() == "Wall (2)");
    CHECK(doc.find(third)->name.view() == "Wall (3)");
    CHECK(fix_scene_lint(doc) == 0);
}

// M-SCENELINT2 step 1: random broken scenes. Whatever mix of empty and repeated names, parents that are missing, itself
// or a loop, references to nothing, zero scales and NaNs a scene carries, the repair leaves it clean, changes it only
// once, keeps every entity and id, keeps the name of an entity under a parent that stays and had no naming finding, and
// the result still saves and loads to the same bytes.
TEST_CASE("fix_scene_lint fuzz: random broken scenes come out clean, once, with every entity kept") {
    u32 broken_scenes = 0;
    for (u32 seed = 1; seed <= 200; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        EditorScene s;
        const u32 n = 2 + static_cast<u32>(rng() % 9);
        const char* const names[] = {"", "A", "A", "B", "Wall", "Wall", "C"};
        for (u32 i = 0; i < n; ++i) s.create_entity(names[rng() % 7]);
        const auto any = [&]() { return s.doc().at(rng() % s.doc().size()).id; };
        for (u32 i = 0, k = static_cast<u32>(rng() % (n + 2)); i < k; ++i) {
            const EntityId id = any();
            switch (rng() % 5) {
                case 0:
                    s.add_component(id, refl::type_of<ChildOf>());
                    s.set(id, ChildOf{doc_entity_ref(rng() % 4 == 0 ? static_cast<EntityId>(900 + rng() % 5)
                                                                    : any())});  // may be itself or close a loop
                    break;
                case 1: {
                    LocalTransform t;
                    t.scale = {1.0f, rng() % 2 ? 0.0f : 2.0f, 1.0f};
                    set_transform(s, id, t);
                    break;
                }
                case 2: {
                    LocalTransform t;
                    t.position.y = std::numeric_limits<f32>::quiet_NaN();
                    set_transform(s, id, t);
                    break;
                }
                case 3: {
                    s.add_component(id, refl::type_of<kit::AttachToBone>());
                    kit::AttachToBone a;
                    a.target = doc_entity_ref(rng() % 2 ? 950 : any());
                    s.set(id, a);
                    break;
                }
                default:
                    s.set_parent(id, any());  // a clean parent link, or a refused cycle
                    break;
            }
        }
        SceneDocument doc = s.doc();
        const Array<LintFinding> found = lint_scene(doc);
        std::vector<EntityId> ids;
        std::vector<std::string> kept_names;
        for (const DocEntity& e : doc.entities()) {
            ids.push_back(e.id);
            // Dropping a parent link makes a root, so a later root may then be renamed; only an entity under a parent
            // that stays (no finding of its own, a parent the scene has) keeps its name for certain.
            bool naming = false;
            const DocComponent* link = e.find(refl::type_of<ChildOf>());
            if (link == nullptr ||
                doc.find(doc_entity_id(static_cast<const ChildOf*>(link->data())->parent)) == nullptr)
                naming = true;
            for (const LintFinding& f : found)
                if (f.id == e.id && (f.kind == LintKind::EmptyName || f.kind == LintKind::DuplicateSiblingName ||
                                     f.kind == LintKind::ParentCycle || f.kind == LintKind::MissingParent))
                    naming = true;
            kept_names.push_back(naming ? std::string() : std::string(e.name.view()));
        }

        const usize fixed = fix_scene_lint(doc);
        CHECK((fixed > 0) == !found.empty());
        if (!found.empty()) ++broken_scenes;
        CHECK(lint_scene(doc).empty());
        REQUIRE(doc.size() == ids.size());
        for (usize i = 0; i < ids.size(); ++i) {
            CHECK(doc.at(i).id == ids[i]);
            if (!kept_names[i].empty()) CHECK(doc.at(i).name.view() == kept_names[i]);
        }
        cook::String once, again;
        doc.serialize(once);
        CHECK(fix_scene_lint(doc) == 0);
        doc.serialize(again);
        CHECK(once.view() == again.view());

        SceneDocument back;
        cook::DiagList diags;
        REQUIRE(back.parse(once.view(), "fuzz", diags));
        cook::String reparsed;
        back.serialize(reparsed);
        CHECK(reparsed.view() == once.view());
    }
    CHECK(broken_scenes > 100);  // the generator does break scenes; a run of tidy ones would prove nothing
}
