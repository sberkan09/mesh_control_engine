#pragma once
// The ImGui side of the inspector's widget seam (editor-tools.md §2.6): one thin adapter from
// DrawCtx to ImGui widgets. The pickers list the project catalog (assets, filtered by type) and
// the open scene (entities) — finding F6's MVP pickers.

#include "mce/editor/editor_session.h"
#include "mce/editor/inspector.h"

namespace mce::editor {

class ImGuiDrawCtx final : public DrawCtx {
public:
    // `filter` (kFilterSize chars) holds the picker filter text; it outlives the per-frame context.
    static constexpr usize kFilterSize = 64;
    ImGuiDrawCtx(EditorSession& session, char* filter) : session_(session), filter_(filter) {}

    bool begin_component(const char* type_name, bool* remove) override;
    void end_component() override;
    bool begin_group(const char* label, bool default_open) override;
    void end_group() override;

    bool text_field(const char* label, char* buf, usize capacity) override;
    bool checkbox(const char* label, bool* v) override;
    bool int_field(const char* label, i64* v, i64 min, i64 max) override;
    bool float_field(const char* label, f64* v, f64 min, f64 max) override;
    bool float_n(const char* label, f32* v, u32 n, f32 min, f32 max) override;
    bool color_field(const char* label, f32* rgba) override;
    bool enum_field(const char* label, i64* v, const refl::EnumRecord& e) override;
    bool asset_field(const char* label, asset::Guid* guid, StringView asset_type) override;
    bool entity_field(const char* label, EntityId* id) override;
    void read_only(const char* label, StringView text) override;
    void help(const char* text) override;
    const refl::TypeRecord* add_component_menu(Span<const refl::TypeRecord* const> addable) override;

private:
    EditorSession& session_;
    char* filter_;
};

}  // namespace mce::editor
