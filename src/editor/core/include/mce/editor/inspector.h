#pragma once
// The reflection-driven inspector (editor-tools.md §2.6, FR-EDT-020): zero per-type code. It walks
// mce_refl's field records and dispatches widgets by FieldKind through DrawCtx — the editor's only
// widget seam — so the round-trip logic runs headless in CI against a scripted DrawCtx, and the
// ImGui panel is one thin adapter. Every committed change goes through EditorScene (never the
// document or the world directly).

#include "mce/editor/editor_scene.h"
#include "mce/math/math.h"

namespace mce::editor {

class DrawCtx {
public:
    virtual ~DrawCtx() = default;

    // Structure. begin_component returns true when its body should be drawn; `remove` is set when
    // the user asked to remove the component.
    virtual bool begin_component(const char* type_name, bool* remove) = 0;
    virtual void end_component() = 0;
    // Nested struct (open by default) or fixed array (collapsed by default).
    virtual bool begin_group(const char* label, bool default_open) = 0;
    virtual void end_group() = 0;

    // Widgets: return true when the user changed the value this frame.
    virtual bool text_field(const char* label, char* buf, usize capacity) = 0;
    virtual bool checkbox(const char* label, bool* v) = 0;
    virtual bool int_field(const char* label, i64* v, i64 min, i64 max) = 0;
    virtual bool float_field(const char* label, f64* v, f64 min, f64 max) = 0;  // min >= max: unbounded
    virtual bool float_n(const char* label, f32* v, u32 n, f32 min, f32 max) = 0;
    virtual bool color_field(const char* label, f32* rgba) = 0;
    virtual bool enum_field(const char* label, i64* v, const refl::EnumRecord& e) = 0;
    // Asset reference (finding F6 MVP picker: GUID + type-filtered list). `asset_type` is the
    // reflected asset type name ("Texture", "Font", ...).
    virtual bool asset_field(const char* label, asset::Guid* guid, StringView asset_type) = 0;
    virtual bool entity_field(const char* label, EntityId* id) = 0;
    virtual void read_only(const char* label, StringView text) = 0;
    virtual void help(const char* text) = 0;  // tooltip for the previous widget

    // Prefab instances (FR-EDT-080). Called before each top-level field of an instance's
    // component: whether the instance overrides it (else it shows the prefab's value).
    virtual void override_mark(bool overridden) { (void)overridden; }
    // In an instance's component (not its placement): offers "revert to the prefab" (when there
    // is something of its own) and "apply to the prefab" (when there is something to apply).
    enum class OverrideAction : u8 { None, Revert, Apply };
    virtual OverrideAction override_actions(bool can_revert, bool can_apply) {
        (void)can_revert;
        (void)can_apply;
        return OverrideAction::None;
    }

    // Offers the components the entity does not have yet; returns the chosen one or null.
    virtual const refl::TypeRecord* add_component_menu(Span<const refl::TypeRecord* const> addable) = 0;
};

// Draws the name, every component and the add-component menu of `id`; commits through `scene`.
// Returns true when anything changed.
bool draw_inspector(DrawCtx& ui, EditorScene& scene, EntityId id);

// Every authored field of the reflected struct `type` at `value`, as the inspector draws a
// component's (no entity behind it: the UI document panel's anchors, styles and layouts). True when
// a widget changed the value.
bool draw_fields(DrawCtx& ui, const refl::TypeRecord& type, void* value);

// Every reflected component type `e` does not carry yet, sorted by name.
void addable_components(const DocEntity& e, Array<const refl::TypeRecord*>& out);

// Rotation shown as Euler degrees (x, y, z), applied in Z * Y * X order; the 2D angle is z.
math::vec3 quat_to_euler_deg(math::quat q);
math::quat euler_deg_to_quat(math::vec3 deg);

}  // namespace mce::editor
