// Undo/redo (FR-EDT-040, editor-tools.md §2.4, ADR-0030 §1–§3), headless: the fuzz gate (N random
// commands, undo all -> byte-identical document, redo all -> the edited document again), drag
// coalescing, edit groups, the bounded history, and a scene re-read from disk as one entry.

#include <doctest/doctest.h>

#include <random>
#include <string>

#include "mce/ecs/transform.h"
#include "mce/editor/editor_scene.h"
#include "mce/editor/gizmo.h"
#include "mce/kit/components.h"
#include "mce/physics/components.h"

using namespace mce;
using namespace mce::editor;

namespace undotest {

struct UndoLink {
    Entity target;
    f32 weight = 1.0f;
    u32 pad_ = 0;
};
MCE_COMPONENT(UndoLink, target, weight)

}  // namespace undotest

namespace {

std::string text_of(const EditorScene& s) {
    cook::String t;
    s.serialize(t);
    return std::string(t.view().data(), t.view().size());
}

EntityId random_entity(EditorScene& s, std::mt19937& rng) {
    if (s.doc().size() == 0) return kNoEntity;
    return s.doc().at(rng() % s.doc().size()).id;
}

// One random authoring command through the funnel.
void random_command(EditorScene& s, std::mt19937& rng) {
    const refl::TypeRecord* comps[] = {&refl::type_of<kit::Sprite>(), &refl::type_of<physics::Collider2D>(),
                                       &refl::type_of<undotest::UndoLink>(), &refl::type_of<LocalTransform>()};
    const EntityId e = random_entity(s, rng);
    switch (rng() % 11) {
        case 0: s.create_entity("E" + std::to_string(rng() % 100), rng() % 2 == 0, static_cast<i32>(rng() % 4) - 1); break;
        case 1:
            if (e) s.destroy_entity(e);
            break;
        case 2:
            if (e) s.rename_entity(e, "R" + std::to_string(rng() % 1000));
            break;
        case 3:
            if (e) s.move_entity(e, rng() % s.doc().size());
            break;
        case 4:
            if (e) s.add_component(e, *comps[rng() % 4]);
            break;
        case 5:
            if (e) s.remove_component(e, *comps[rng() % 4]);
            break;
        case 6:
            if (const LocalTransform* t = e ? s.get<LocalTransform>(e) : nullptr) {
                LocalTransform v = *t;
                v.position.x += static_cast<f32>(static_cast<i32>(rng() % 200) - 100) * 0.25f;
                v.rotation = rotation_z(static_cast<f32>(rng() % 628) * 0.01f);
                s.set(e, v);
            }
            break;
        case 7:
            if (const undotest::UndoLink* l = e ? s.get<undotest::UndoLink>(e) : nullptr) {
                undotest::UndoLink v = *l;
                v.target = doc_entity_ref(random_entity(s, rng));
                v.weight = static_cast<f32>(rng() % 10);
                s.set(e, v);
            }
            break;
        case 8:
            if (e) {
                const EntityId ids[] = {e};
                s.duplicate_entities(Span<const EntityId>(ids, 1));
            }
            break;
        case 9:
            if (e) {  // one grouped edit with several structural changes: reorder, delete two
                s.begin_edit("Group");
                s.move_entity(e, rng() % s.doc().size());
                s.destroy_entity(random_entity(s, rng));
                if (const EntityId f = random_entity(s, rng)) s.destroy_entity(f);
                s.end_edit();
            }
            break;
        default:
            if (const kit::Sprite* sp = e ? s.get<kit::Sprite>(e) : nullptr) {
                kit::Sprite v = *sp;
                v.order = static_cast<i32>(rng() % 50);
                v.size = {1.0f + static_cast<f32>(rng() % 4), 1.0f};
                s.set(e, v);
            }
            break;
    }
}

}  // namespace

TEST_CASE("undo fuzz: N random commands, undo all -> byte-identical; redo all -> the edited scene") {
    for (u32 seed = 1; seed <= 40; ++seed) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        EditorScene s;
        for (int i = 0; i < 6; ++i) random_command(s, rng);  // some starting content
        s.clear_undo();
        const std::string start = text_of(s);

        std::vector<std::string> states{start};
        for (int i = 0; i < 200; ++i) {
            const usize before = s.undo_count();
            s.seal_undo();  // separate commands, as separate clicks are
            random_command(s, rng);
            if (s.undo_count() != before) states.push_back(text_of(s));
            else CHECK(text_of(s) == states.back());
        }
        const std::string end = text_of(s);
        REQUIRE(s.verify_world());

        // Undo walks back through every intermediate state.
        usize k = states.size() - 1;
        while (s.can_undo()) {
            REQUIRE(s.undo());
            --k;
            CHECK(text_of(s) == states[k]);
        }
        CHECK(k == 0);
        CHECK(text_of(s) == start);
        CHECK(s.verify_world());
        while (s.can_redo()) REQUIRE(s.redo());
        CHECK(text_of(s) == end);
        CHECK(s.verify_world());

        // Undo half, then a new command drops the redo branch.
        for (int i = 0; i < 20 && s.can_undo(); ++i) s.undo();
        CHECK(s.can_redo());
        s.create_entity("Fresh");
        CHECK_FALSE(s.can_redo());
        CHECK(s.verify_world());
    }
}

TEST_CASE("undo: a drag of one component coalesces until sealed; different components do not merge") {
    EditorScene s;
    const EntityId a = s.create_entity("A");
    const EntityId b = s.create_entity("B");
    s.clear_undo();
    s.seal_undo();
    for (int i = 1; i <= 30; ++i) {  // one inspector drag: 30 frames of the same field
        LocalTransform t = *s.get<LocalTransform>(a);
        t.position.x = static_cast<f32>(i) * 0.1f;
        s.set(a, t);
    }
    CHECK(s.undo_count() == 1);
    LocalTransform tb = *s.get<LocalTransform>(b);
    tb.position.y = 5.0f;
    s.set(b, tb);  // another entity: a new entry
    CHECK(s.undo_count() == 2);
    s.seal_undo();  // mouse released
    tb.position.y = 6.0f;
    s.set(b, tb);
    CHECK(s.undo_count() == 3);
    s.undo();
    s.undo();
    CHECK(s.get<LocalTransform>(b)->position.y == 0.0f);
    s.undo();
    CHECK(s.get<LocalTransform>(a)->position.x == 0.0f);  // the whole drag in one step
    CHECK_FALSE(s.can_undo());
}

TEST_CASE("undo: an edit group is one entry; a gizmo drag of a multi-selection undoes in one step") {
    EditorScene s;
    const EntityId a = s.create_entity("A");
    const EntityId b = s.create_entity("B");
    s.clear_undo();
    const std::string before = text_of(s);
    s.selection().set(a);
    s.selection().toggle(b);
    TranslateGizmo g;
    s.begin_edit("Move");
    REQUIRE(g.begin(s, GizmoPart::XY, {0, 0}));
    for (int i = 1; i <= 10; ++i) g.update(s, {static_cast<f32>(i), 0.5f * static_cast<f32>(i)}, 0.0f);
    g.end();
    s.end_edit();
    CHECK(s.undo_count() == 1);
    CHECK(s.undo_label() == "Move");
    CHECK(s.get<LocalTransform>(b)->position.x == 10.0f);
    s.undo();
    CHECK(text_of(s) == before);
    CHECK(s.redo_label() == "Move");

    // A cancelled drag (Esc) restores everything: no entry at all.
    s.begin_edit("Move");
    REQUIRE(g.begin(s, GizmoPart::X, {0, 0}));
    g.update(s, {3, 0}, 0.0f);
    g.cancel(s);
    s.end_edit();
    CHECK(s.undo_count() == 0);
    CHECK(s.can_redo());  // nothing was pushed, so the redo branch survives
    CHECK_FALSE(s.undo());  // ...and there is nothing to undo
    s.begin_edit("Busy");
    CHECK_FALSE(s.redo());  // never while an edit is open
    s.end_edit();
}

TEST_CASE("undo: delete restores the entity, its place and the references that were nulled") {
    EditorScene s;
    const EntityId a = s.create_entity("A");
    const EntityId b = s.create_entity("B");
    const EntityId c = s.create_entity("C");
    s.add_component(b, refl::type_of<undotest::UndoLink>());
    s.set(b, undotest::UndoLink{doc_entity_ref(a), 2.0f});
    s.add_component(c, refl::type_of<undotest::UndoLink>());
    s.set(c, undotest::UndoLink{doc_entity_ref(a), 3.0f});
    s.selection().set(a);
    const std::string before = text_of(s);
    REQUIRE(s.destroy_entity(a));
    CHECK(s.get<undotest::UndoLink>(b)->target.is_null());
    REQUIRE(s.undo());
    CHECK(text_of(s) == before);
    CHECK(s.doc().index_of(a) == 0);
    CHECK(s.get<undotest::UndoLink>(c)->target == doc_entity_ref(a));
    CHECK(s.verify_world());
    // The mirror maps the restored reference to the restored entity's new runtime handle.
    const Entity live = s.world().try_get<undotest::UndoLink>(s.runtime_entity(b))->target;
    CHECK(live == s.runtime_entity(a));
    REQUIRE(s.redo());
    CHECK_FALSE(s.doc().find(a));
    CHECK_FALSE(s.selection().contains(a));
}

TEST_CASE("undo: history is bounded; opening a scene clears it") {
    EditorScene s;
    const EntityId a = s.create_entity("A");
    for (usize i = 0; i < EditorScene::kMaxUndoEntries + 40; ++i) {
        s.seal_undo();
        s.rename_entity(a, "N" + std::to_string(i));
    }
    CHECK(s.undo_count() == EditorScene::kMaxUndoEntries);
    CHECK(s.undo_bytes() > 0);
    CHECK(s.undo_bytes() <= EditorScene::kMaxUndoBytes);
    s.new_scene("other");
    CHECK_FALSE(s.can_undo());
    CHECK(s.undo_bytes() == 0);
}

TEST_CASE("undo: a scene re-read from disk (an MCP client saved it) is one undoable entry") {
    EditorScene s;
    const EntityId a = s.create_entity("A");
    s.create_entity("B");
    s.clear_undo();
    const std::string mine = text_of(s);

    // The same scene as another process left it: B renamed, C added, A moved.
    EditorScene other;
    cook::DiagList diags;
    REQUIRE(other.recover(StringView(mine.data(), mine.size()), diags));
    other.rename_entity(other.doc().at(1).id, "B2");
    other.create_entity("C");
    LocalTransform t;
    t.position = {4, 0, 0};
    other.set(other.doc().at(0).id, t);
    const std::string theirs = text_of(other);

    s.selection().set(a);
    REQUIRE(s.reload_text(StringView(theirs.data(), theirs.size()), "disk", diags));
    CHECK(text_of(s) == theirs);
    CHECK(s.undo_count() == 1);
    CHECK(s.undo_label() == "Reload from disk");
    CHECK(s.verify_world());
    REQUIRE(s.undo());
    CHECK(text_of(s) == mine);
    REQUIRE(s.redo());
    CHECK(text_of(s) == theirs);

    // A different scene (other GUID) replaces the document and starts a fresh history.
    EditorScene third;
    third.create_entity("Z");
    const std::string z = text_of(third);
    REQUIRE(s.reload_text(StringView(z.data(), z.size()), "disk", diags));
    CHECK_FALSE(s.can_undo());
    CHECK(text_of(s) == z);
    CHECK_FALSE(s.reload_text("{ not json", "disk", diags));
}
