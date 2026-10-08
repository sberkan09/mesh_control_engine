#include "imgui_draw_ctx.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>

namespace mce::editor {

namespace {

constexpr f32 kDragSpeed = 0.01f;

bool contains_ci(StringView hay, StringView needle) {
    if (needle.empty()) return true;
    for (usize i = 0; i + needle.size() <= hay.size(); ++i) {
        usize k = 0;
        while (k < needle.size()) {
            char a = hay[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) break;
            ++k;
        }
        if (k == needle.size()) return true;
    }
    return false;
}

}  // namespace

bool ImGuiDrawCtx::begin_component(const char* type_name, bool* remove) {
    ImGui::PushID(type_name);
    const f32 right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const bool open = ImGui::CollapsingHeader(type_name, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);
    if (ImGui::BeginPopupContextItem("##component")) {
        if (ImGui::MenuItem("Remove component")) *remove = true;
        ImGui::EndPopup();
    }
    // A small remove button at the right end of the header.
    ImGui::SameLine(right - ImGui::GetFrameHeight());
    if (ImGui::SmallButton("x")) *remove = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove %s", type_name);
    if (!open) {
        ImGui::PopID();
        return false;
    }
    ImGui::Indent(ImGui::GetStyle().IndentSpacing * 0.25f);
    return true;
}

void ImGuiDrawCtx::end_component() {
    ImGui::Unindent(ImGui::GetStyle().IndentSpacing * 0.25f);
    ImGui::PopID();
}

bool ImGuiDrawCtx::begin_group(const char* label, bool default_open) {
    return ImGui::TreeNodeEx(label, default_open ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None);
}

void ImGuiDrawCtx::end_group() { ImGui::TreePop(); }

bool ImGuiDrawCtx::text_field(const char* label, char* buf, usize capacity) {
    return ImGui::InputText(label, buf, capacity);
}

bool ImGuiDrawCtx::checkbox(const char* label, bool* v) { return ImGui::Checkbox(label, v); }

bool ImGuiDrawCtx::int_field(const char* label, i64* v, i64 min, i64 max) {
    return ImGui::DragScalar(label, ImGuiDataType_S64, v, 0.2f, &min, &max);
}

bool ImGuiDrawCtx::float_field(const char* label, f64* v, f64 min, f64 max) {
    const bool bounded = min < max;
    return ImGui::DragScalar(label, ImGuiDataType_Double, v, kDragSpeed, bounded ? &min : nullptr,
                             bounded ? &max : nullptr, "%.3f");
}

bool ImGuiDrawCtx::float_n(const char* label, f32* v, u32 n, f32 min, f32 max) {
    const bool bounded = min < max;
    return ImGui::DragScalarN(label, ImGuiDataType_Float, v, static_cast<int>(n), kDragSpeed, bounded ? &min : nullptr,
                              bounded ? &max : nullptr, "%.3f");
}

bool ImGuiDrawCtx::color_field(const char* label, f32* rgba) {
    return ImGui::ColorEdit4(label, rgba, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaPreviewHalf);
}

bool ImGuiDrawCtx::enum_field(const char* label, i64* v, const refl::EnumRecord& e) {
    const refl::EnumValue* cur = e.find(*v);
    char fallback[32];
    std::snprintf(fallback, sizeof(fallback), "%lld", static_cast<long long>(*v));
    bool changed = false;
    if (ImGui::BeginCombo(label, cur ? cur->name_str : fallback)) {
        for (const refl::EnumValue& ev : e.values) {
            const bool sel = ev.value == *v;
            if (ImGui::Selectable(ev.name_str, sel) && !sel) {
                *v = ev.value;
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool ImGuiDrawCtx::asset_field(const char* label, asset::Guid* guid, StringView asset_type) {
    const EditorProject& project = session_.project();
    const ProjectAsset* cur = guid->is_null() ? nullptr : project.find(*guid);
    FixedString<160> preview;
    if (guid->is_null()) preview.append("None");
    else if (cur) preview.append(cur->path.view());
    else {
        preview.append("missing ");
        preview.append(asset::to_string(*guid).view());
    }
    bool changed = false;
    if (ImGui::BeginCombo(label, preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) {
            filter_[0] = 0;
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##filter", "filter", filter_, kFilterSize);
        if (ImGui::Selectable("None", guid->is_null()) && !guid->is_null()) {
            *guid = asset::Guid{};
            changed = true;
        }
        for (const ProjectAsset& a : project.assets()) {
            if (!asset_type.empty() && asset_type != StringView(asset_type_name(a.type))) continue;
            if (!contains_ci(a.path.view(), StringView(filter_))) continue;
            const bool sel = a.guid == *guid;
            if (ImGui::Selectable(a.path.c_str(), sel) && !sel) {
                *guid = a.guid;
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", asset::to_string(a.guid).c_str());
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool ImGuiDrawCtx::entity_field(const char* label, EntityId* id) {
    const SceneDocument& doc = session_.scene().doc();
    const DocEntity* cur = *id == kNoEntity ? nullptr : doc.find(*id);
    FixedString<96> preview;
    if (*id == kNoEntity) preview.append("None");
    else if (cur) preview.append(cur->name.view());
    else preview.append("missing");
    bool changed = false;
    if (ImGui::BeginCombo(label, preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("None", *id == kNoEntity) && *id != kNoEntity) {
            *id = kNoEntity;
            changed = true;
        }
        for (const DocEntity& e : doc.entities()) {
            ImGui::PushID(static_cast<int>(e.id));
            const bool sel = e.id == *id;
            if (ImGui::Selectable(e.name.view().empty() ? "(unnamed)" : e.name.c_str(), sel) && !sel) {
                *id = e.id;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void ImGuiDrawCtx::read_only(const char* label, StringView text) {
    FixedString<256> t;
    t.append(text);
    ImGui::LabelText(label, "%s", t.c_str());
}

void ImGuiDrawCtx::help(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", text);
}

const refl::TypeRecord* ImGuiDrawCtx::add_component_menu(Span<const refl::TypeRecord* const> addable) {
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Add Component", ImVec2(-1.0f, 0.0f))) {
        filter_[0] = 0;
        ImGui::OpenPopup("##add_component");
    }
    const refl::TypeRecord* chosen = nullptr;
    if (ImGui::BeginPopup("##add_component")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        ImGui::InputTextWithHint("##filter", "filter", filter_, kFilterSize);
        for (const refl::TypeRecord* t : addable) {
            if (!contains_ci(StringView(t->name), StringView(filter_))) continue;
            if (ImGui::Selectable(t->name)) chosen = t;
        }
        if (addable.empty()) ImGui::TextDisabled("Every component is already on this entity.");
        ImGui::EndPopup();
    }
    return chosen;
}

}  // namespace mce::editor
