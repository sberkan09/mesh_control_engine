// Inspector sweep (M-EDITUX step 3), headless: every component type the editor offers is added to an
// entity, every field is nudged through a scripted DrawCtx, and the result must undo to the byte,
// survive serialize -> parse -> serialize and leave the preview world consistent.

#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "mce/editor/editor_scene.h"
#include "mce/editor/inspector.h"

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

}  // namespace

TEST_CASE("inspector sweep: every offered component takes a nudge of every field, undoes and round-trips") {
    EditorScene probe;
    const EntityId bare = probe.create_entity("Bare", false);
    NudgeCtx menu;
    draw_inspector(menu, probe, bare);
    REQUIRE(menu.addable.size() > 20);
    for (const refl::TypeRecord* t : menu.addable)
        CHECK_MESSAGE(std::string(t->name) != "WorldTransform", "a derived component is not offered");

    u32 nudged_types = 0;
    for (const refl::TypeRecord* type : menu.addable) {
        const std::string tname = type->name;
        CAPTURE(tname);
        EditorScene s;
        const EntityId id = s.create_entity("E", false);
        REQUIRE(s.add_component(id, *type));
        s.clear_undo();
        const std::string before = text_of(s);

        NudgeCtx ui;
        const bool changed = draw_inspector(ui, s, id);
        CHECK(s.verify_world());
        if (!changed) {
            CHECK(text_of(s) == before);
            continue;
        }
        ++nudged_types;
        const std::string after = text_of(s);

        // The edited document is a valid file: it parses and re-serializes to the same bytes.
        SceneDocument back;
        cook::DiagList diags;
        REQUIRE(back.parse(StringView(after.data(), after.size()), "sweep", diags));
        cook::String again;
        back.serialize(again);
        CHECK(std::string(again.view().data(), again.view().size()) == after);

        // Undo takes every nudge back; redo brings them again.
        while (s.can_undo()) REQUIRE(s.undo());
        CHECK(text_of(s) == before);
        CHECK(s.verify_world());
        while (s.can_redo()) REQUIRE(s.redo());
        CHECK(text_of(s) == after);
        CHECK(s.verify_world());
    }
    CHECK(nudged_types > 10);
}
