// The multi-entity editor commands under undo (M-EDITUX2 step 1), headless: group, ungroup, align,
// distribute, flip, snap, match, reset and duplicate-with-offset run in random sequences over random selections
// (parents with their children, empty and single selections, an id that was deleted a while ago). After every command
// the document is one history entry or none; undoing everything is byte-identical, redoing everything returns the
// edited text, and the preview world agrees with the document.

#include <doctest/doctest.h>

#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "mce/containers/array.h"
#include "mce/ecs/transform.h"
#include "mce/editor/align_entities.h"
#include "mce/editor/duplicate_offset.h"
#include "mce/editor/editor_scene.h"
#include "mce/editor/flip_entities.h"
#include "mce/editor/gizmo.h"
#include "mce/editor/group_entities.h"
#include "mce/editor/inspector.h"
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

// Nudges every field it is shown, to a value inside the field's bounds, and records the menu.
class NudgeCtx final : public DrawCtx {
public:
    std::vector<const refl::TypeRecord*> addable;
    u32 nudged = 0;

    bool begin_component(const char*, bool*) override { return true; }
    void end_component() override {}
    bool begin_group(const char*, bool) override { return true; }
    void end_group() override {}
    bool text_field(const char*, char* buf, usize cap) override {
        std::snprintf(buf, cap, "%s", "nudged");
        return ++nudged, true;
    }
    bool checkbox(const char*, bool* v) override {
        *v = !*v;
        return ++nudged, true;
    }
    bool int_field(const char*, i64* v, i64 lo, i64 hi) override {
        const i64 next = *v + 1;
        *v = (lo < hi && next > hi) ? lo : next;
        return ++nudged, true;
    }
    bool float_field(const char*, f64* v, f64 lo, f64 hi) override {
        const f64 next = *v + 0.5;
        *v = (lo < hi && next > hi) ? lo : next;
        return ++nudged, true;
    }
    bool float_n(const char*, f32* v, u32 n, f32 lo, f32 hi) override {
        for (u32 i = 0; i < n; ++i) {
            const f32 next = v[i] + 0.5f;
            v[i] = (lo < hi && next > hi) ? lo : next;
        }
        return ++nudged, true;
    }
    bool color_field(const char*, f32* rgba) override {
        rgba[0] = rgba[0] > 0.5f ? 0.25f : 0.75f;
        return ++nudged, true;
    }
    bool enum_field(const char*, i64* v, const refl::EnumRecord& e) override {
        for (const refl::EnumValue& ev : e.values)
            if (ev.value != *v) {
                *v = ev.value;
                return ++nudged, true;
            }
        return false;
    }
    bool asset_field(const char*, asset::Guid*, StringView) override { return false; }
    bool entity_field(const char*, EntityId*) override { return false; }
    void read_only(const char*, StringView) override {}
    void help(const char*) override {}
    const refl::TypeRecord* add_component_menu(Span<const refl::TypeRecord* const> list) override {
        addable.assign(list.begin(), list.end());
        return nullptr;
    }
};

struct Run {
    EditorScene s;
    std::mt19937 rng;
    std::vector<EntityId> dead;  // ids that once existed

    std::vector<const refl::TypeRecord*> addable;

    explicit Run(u32 seed) : rng(seed) {
        {
            EditorScene probe;
            NudgeCtx menu;
            draw_inspector(menu, probe, probe.create_entity("Bare", false));
            addable = menu.addable;
        }
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

    // The M-EDITUX4 mix: component add/remove, an inspector nudge, a gizmo drag (committed or cancelled), reordering,
    // between the commands above.
    void ux_command() {
        if (rng() % 2 || s.doc().size() == 0) {
            mixed_command();
            return;
        }
        std::vector<EntityId> before;
        for (const DocEntity& e : s.doc().entities()) before.push_back(e.id);
        const EntityId id = s.doc().at(rng() % s.doc().size()).id;
        switch (rng() % 6) {
            case 0:
                s.add_component(id, *addable[rng() % addable.size()]);
                break;
            case 1:
                s.remove_component(id, *addable[rng() % addable.size()]);
                break;
            case 2: {
                NudgeCtx ui;
                s.begin_edit("Nudge");  // one entry for the whole panel pass, as a frame's edits are in the editor
                draw_inspector(ui, s, id);
                s.end_edit();
                break;
            }
            case 3:
                s.move_entity(id, rng() % s.doc().size());
                break;
            default: {  // a drag over a random selection
                s.selection().clear();
                const u32 n = 1 + static_cast<u32>(rng() % 3);
                for (u32 i = 0; i < n; ++i) s.selection().toggle(s.doc().at(rng() % s.doc().size()).id);
                TransformGizmo g;
                g.set_mode(static_cast<GizmoMode>(rng() % 3));
                const GizmoPart parts[] = {GizmoPart::XY, GizmoPart::X, GizmoPart::Y, GizmoPart::Ring};
                const math::vec2 from{static_cast<f32>(rng() % 20) - 10.0f, static_cast<f32>(rng() % 20) - 10.0f};
                const math::vec2 to{static_cast<f32>(rng() % 20) - 10.0f, static_cast<f32>(rng() % 20) - 10.0f};
                s.begin_edit("Drag");
                if (g.begin(s, parts[rng() % 4], from)) {
                    g.update(s, to, rng() % 2 ? 0.0f : 1.0f);
                    if (rng() % 4 == 0)
                        g.cancel(s);
                    else
                        g.end();
                }
                s.end_edit();
                break;
            }
        }
        for (const EntityId e : before)
            if (!s.doc().find(e)) dead.push_back(e);
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
void fuzz(int mode) {
    for (u32 seed = 1; seed <= 30; ++seed) {
        CAPTURE(seed);
        Run r(seed);
        const std::string start = text_of(r.s);
        std::vector<std::string> states{start};
        for (u32 i = 0; i < 120; ++i) {
            CAPTURE(i);
            const usize before = r.s.undo_count();
            r.s.seal_undo();
            if (mode == 2)
                r.ux_command();
            else if (mode == 1)
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
    fuzz(0);
}

TEST_CASE("multi-entity commands fuzz, mixed with create, delete, reparent and paste") { fuzz(1); }

TEST_CASE("multi-entity commands fuzz, with component add/remove, inspector edits and gizmo drags") { fuzz(2); }
