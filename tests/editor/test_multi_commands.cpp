// The multi-entity editor commands under undo (M-EDITUX2 step 1), headless: group, ungroup, align,
// distribute, flip, snap, match, reset and duplicate-with-offset run in random sequences over random selections
// (parents with their children, empty and single selections, an id that was deleted a while ago). After every command
// the document is one history entry or none; undoing everything is byte-identical, redoing everything returns the
// edited text, and the preview world agrees with the document.

#include <doctest/doctest.h>

#include <random>
#include <string>
#include <vector>

#include "mce/containers/array.h"
#include "mce/ecs/transform.h"
#include "mce/editor/align_entities.h"
#include "mce/editor/duplicate_offset.h"
#include "mce/editor/editor_scene.h"
#include "mce/editor/flip_entities.h"
#include "mce/editor/group_entities.h"
#include "mce/editor/match_transform.h"
#include "mce/editor/reset_transform.h"
#include "mce/editor/snap_entities.h"

using namespace mce;
using namespace mce::editor;

namespace {

std::string text_of(const EditorScene& s) {
    cook::String t;
    s.serialize(t);
    return std::string(t.view().data(), t.view().size());
}

struct Run {
    EditorScene s;
    std::mt19937 rng;
    std::vector<EntityId> dead;  // ids that once existed

    explicit Run(u32 seed) : rng(seed) {
        for (u32 i = 0; i < 8; ++i) {
            const EntityId e = s.create_entity("E" + std::to_string(i));
            LocalTransform t = *s.get<LocalTransform>(e);
            t.position = {static_cast<f32>(rng() % 40) - 20.0f, static_cast<f32>(rng() % 40) - 20.0f, 0.0f};
            s.set(e, t);
        }
        for (u32 i = 1; i < 8; i += 2) s.set_parent(s.doc().at(i).id, s.doc().at(i - 1).id);  // four parent/child pairs
        s.clear_undo();
    }

    // 0 to 4 entities, sometimes a stale id.
    Array<EntityId> pick() {
        Array<EntityId> ids;
        const u32 n = static_cast<u32>(rng() % 5);
        for (u32 i = 0; i < n && s.doc().size() > 0; ++i) ids.push_back(s.doc().at(rng() % s.doc().size()).id);
        if (!dead.empty() && rng() % 8 == 0) ids.push_back(dead[rng() % dead.size()]);
        return ids;
    }

    void command() {
        const Array<EntityId> ids = pick();
        const Span<const EntityId> sel(ids.data(), ids.size());
        const AlignAxis axis = rng() % 2 ? AlignAxis::X : AlignAxis::Y;
        // Remember what existed, so a deleted group or ungrouped parent becomes a stale id later.
        std::vector<EntityId> before;
        for (const DocEntity& e : s.doc().entities()) before.push_back(e.id);
        switch (rng() % 10) {
            case 0:
                group_entities(s, sel);
                break;
            case 1:
                ungroup_entities(s, sel);
                break;
            case 2:
                align_entities(s, sel, axis, static_cast<AlignMode>(rng() % 3));
                break;
            case 3:
                distribute_entities(s, sel, axis);
                break;
            case 4:
                flip_entities(s, sel, axis);
                break;
            case 5:
                snap_entities(s, sel, 0.5f + static_cast<f32>(rng() % 4));
                break;
            case 6:
                if (!ids.empty()) match_transforms(s, sel, ids[0], static_cast<MatchPart>(rng() % 3));
                break;
            case 7:
                reset_transforms(s, sel);
                break;
            case 8:
                duplicate_offset(s, sel, 1.0f, -2.0f);
                break;
            default: {  // a plain edit between them, so the history is not only these commands
                if (s.doc().size() > 0)
                    s.rename_entity(s.doc().at(rng() % s.doc().size()).id, "R" + std::to_string(rng() % 100));
                break;
            }
        }
        for (const EntityId id : before)
            if (!s.doc().find(id)) dead.push_back(id);
    }

    // The M-EDITUX3 mix: structural edits (create, delete, reparent, paste) between the selection commands.
    void mixed_command() {
        if (rng() % 2) {
            command();
            return;
        }
        std::vector<EntityId> before;
        for (const DocEntity& e : s.doc().entities()) before.push_back(e.id);
        const auto any = [&]() { return s.doc().at(rng() % s.doc().size()).id; };
        switch (s.doc().size() == 0 ? 0u : rng() % 5) {
            case 0:
                s.create_entity("N" + std::to_string(rng() % 100));
                break;
            case 1:
                s.destroy_entity(any());
                break;
            case 2:
                s.set_parent(any(),
                             rng() % 3 ? any() : kNoEntity);  // may refuse a cycle; a refusal must leave no entry
                break;
            case 3: {
                const Array<EntityId> ids = pick();
                cook::String text;
                s.copy_entities(Span<const EntityId>(ids.data(), ids.size()), text);
                cook::DiagList diags;
                s.paste_entities(text.view(), -1, diags);
                break;
            }
            default:
                command();
                break;
        }
        for (const EntityId id : before)
            if (!s.doc().find(id)) dead.push_back(id);
    }
};

}  // namespace

namespace {
void fuzz(bool mixed) {
    for (u32 seed = 1; seed <= 30; ++seed) {
        CAPTURE(seed);
        Run r(seed);
        const std::string start = text_of(r.s);
        std::vector<std::string> states{start};
        for (u32 i = 0; i < 120; ++i) {
            CAPTURE(i);
            const usize before = r.s.undo_count();
            r.s.seal_undo();
            if (mixed)
                r.mixed_command();
            else
                r.command();
            if (r.s.undo_count() != before)
                states.push_back(text_of(r.s));
            else
                REQUIRE(text_of(r.s) == states.back());  // no history entry means nothing changed
            REQUIRE(r.s.verify_world());
        }
        CHECK(states.size() > 40);  // the commands do act: a run that mostly no-ops would prove nothing
        const std::string end = text_of(r.s);

        usize k = states.size() - 1;
        while (r.s.can_undo()) {
            REQUIRE(r.s.undo());
            --k;
            CHECK(text_of(r.s) == states[k]);
        }
        CHECK(k == 0);
        CHECK(text_of(r.s) == start);
        CHECK(r.s.verify_world());
        while (r.s.can_redo()) REQUIRE(r.s.redo());
        CHECK(text_of(r.s) == end);
        CHECK(r.s.verify_world());
    }
}
}  // namespace

TEST_CASE(
    "multi-entity commands fuzz: every command is one undo entry or none; undo all is byte-identical, redo all the "
    "edit") {
    fuzz(false);
}

TEST_CASE("multi-entity commands fuzz, mixed with create, delete, reparent and paste") { fuzz(true); }
