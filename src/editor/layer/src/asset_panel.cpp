// The Assets panel (ROADMAP M6, editor-tools.md §2.5, ADR-0030 §7): the project catalog as a
// folder tree and a filtered list. Drag an asset onto the viewport (a texture becomes a sprite, a
// prefab an instance, a scene opens), onto a hierarchy row (a child of it) or onto an inspector
// asset field. Rename / move keeps the GUID (FR-EDT-110); dropping an asset on a folder moves it.

#include <imgui.h>

#include <cstdio>
#include <cstring>

#include "mce/editor/asset_browser.h"
#include "mce/editor/editor_shell.h"
#include "mce/log.h"
#include "scene_panels.h"

namespace mce::editor {
namespace {

class AssetPanel final : public IEditorPanel {
public:
    const char* name() const override { return "Assets"; }

    void draw(EditorShell& shell) override {
        EditorSession& session = shell.session();
        const EditorProject& project = session.project();
        if (!project.is_open()) {
            ImGui::TextDisabled("Open a project to browse its assets.");
            return;
        }
        if (project.assets().size() != seen_assets_ || project.generation() != seen_generation_) {
            model_.refresh(project);
            seen_assets_ = project.assets().size();
            seen_generation_ = project.generation();
        }

        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::InputTextWithHint("##filter", "search all folders", filter_, sizeof(filter_))) model_.set_filter(filter_);
        ImGui::SameLine();
        static constexpr const char* kTypes[] = {"All", "Texture", "Prefab", "Scene", "Material", "Sfx", "Font", "Mesh", "Tilemap", "ParticlePreset", "Sequence", "Timeline", "SpriteSheet", "AnimGraph", "UiDocument", "UiTheme", "NavMesh"};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        if (ImGui::Combo("##type", &type_, kTypes, IM_ARRAYSIZE(kTypes))) model_.set_type(type_ == 0 ? StringView() : StringView(kTypes[type_]));

        const f32 tree_w = ImGui::GetFontSize() * 10.0f;
        if (ImGui::BeginChild("##folders", ImVec2(tree_w, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX)) {
            draw_folder(session, StringView(), "assets");
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##entries")) draw_entries(shell);
        ImGui::EndChild();
    }

private:
    void draw_folder(EditorSession& session, StringView folder, const char* label) {
        Array<StringView> subs;
        model_.subfolders(folder, subs);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (folder.empty()) flags |= ImGuiTreeNodeFlags_DefaultOpen;
        if (subs.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (model_.filter().empty() && model_.folder() == folder) flags |= ImGuiTreeNodeFlags_Selected;
        ImGui::PushID(static_cast<int>(folder.size()));
        // The root folder is an empty view whose data() may be null, which ImGui cannot hash.
        if (folder.empty()) ImGui::PushID("/");
        else ImGui::PushID(folder.data(), folder.data() + folder.size());
        const bool expanded = ImGui::TreeNodeEx("##folder", flags, "%s", label);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            model_.set_folder(folder);
            filter_[0] = 0;
            model_.set_filter(StringView());
        }
        // An asset dropped on a folder moves there (same file name, same GUID).
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                asset::Guid g;
                std::memcpy(&g, p->Data, sizeof(g));
                if (const ProjectAsset* a = session.project().find(g)) {
                    cook::String to(folder);
                    if (!to.empty()) to += "/";
                    to += asset_file_name(a->path.view());
                    move(session, g, to.view());
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (expanded) {
            for (StringView sub : subs) {
                const FixedString<128> name(asset_file_name(sub));
                draw_folder(session, sub, name.c_str());
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
        ImGui::PopID();
    }

    void draw_entries(EditorShell& shell) {
        EditorSession& session = shell.session();
        Array<const ProjectAsset*> list;
        model_.entries(session.project(), list);
        if (list.empty()) ImGui::TextDisabled(model_.filter().empty() ? "(empty folder)" : "(no match)");
        asset::Guid open_scene;
        for (const ProjectAsset* a : list) {
            ImGui::PushID(a);
            const char* type = asset_type_name(a->type);
            const StringView shown = model_.filter().empty() ? asset_file_name(a->path.view()) : a->path.view();
            if (renaming_ == a->guid) {
                draw_rename(session, *a);
            } else {
                const FixedString<200> label(shown);
                ImGui::Selectable(label.c_str(), selected_ == a->guid, ImGuiSelectableFlags_AllowDoubleClick);
                if (ImGui::IsItemClicked()) selected_ = a->guid;
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    // A scene opens; so does a prefab, for editing the asset itself (FR-EDT-080).
                    if (asset_drop_kind(a->type) == AssetDrop::OpenScene || a->type == asset::ChunkType::Prefab)
                        open_scene = a->guid;
                    // A UI document opens in the UI Document panel.
                    if (a->type == asset::ChunkType::UiDocument) shell.open_ui_document(a->guid);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s  %s\n%s", type, a->path.c_str(), asset::to_string(a->guid).c_str());
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload(kAssetPayload, &a->guid, sizeof(a->guid));
                    ImGui::Text("%s  %s", type, label.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginPopupContextItem("##ctx")) {
                    selected_ = a->guid;
                    if (ImGui::MenuItem("Rename / move", "F2")) begin_rename(*a);
                    const AssetDrop kind = asset_drop_kind(a->type);
                    if (ImGui::MenuItem("Add to scene", nullptr, false,
                                        !shell.playing() && (kind == AssetDrop::Sprite || kind == AssetDrop::PrefabInstance)))
                        drop_asset(shell.scene(), *a, shell.camera().center);
                    if (ImGui::MenuItem("Open scene", nullptr, false, kind == AssetDrop::OpenScene)) open_scene = a->guid;
                    if (kind == AssetDrop::PrefabInstance && ImGui::MenuItem("Open prefab")) open_scene = a->guid;
                    if (ImGui::MenuItem("Copy GUID")) ImGui::SetClipboardText(asset::to_string(a->guid).c_str());
                    ImGui::EndPopup();
                }
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(type).x);
                ImGui::TextDisabled("%s", type);
            }
            ImGui::PopID();
        }
        if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F2, false))
            if (const ProjectAsset* a = session.project().find(selected_)) begin_rename(*a);
        if (!open_scene.is_null())
            if (const ProjectAsset* a = session.project().find(open_scene)) shell.request_open_scene(a->path.view());
    }

    void begin_rename(const ProjectAsset& a) {
        renaming_ = a.guid;
        std::snprintf(rename_buf_, sizeof(rename_buf_), "%s", a.path.c_str());
        focus_rename_ = true;
    }

    void draw_rename(EditorSession& session, const ProjectAsset& a) {
        if (focus_rename_) {
            ImGui::SetKeyboardFocusHere();
            focus_rename_ = false;
        }
        ImGui::SetNextItemWidth(-1.0f);
        const bool enter = ImGui::InputText("##rename", rename_buf_, sizeof(rename_buf_), ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter) {
            const asset::Guid g = a.guid;
            renaming_ = asset::Guid{};
            if (!(StringView(rename_buf_) == a.path.view())) move(session, g, StringView(rename_buf_));
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (!ImGui::IsItemActive() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))) {
            renaming_ = asset::Guid{};
        }
    }

    void move(EditorSession& session, asset::Guid g, StringView to) {
        if (Result<void> r = session.rename_asset(g, to); !r) {
            FixedString<512> msg;
            format_error(r.error(), msg);
            MCE_LOG_ERROR("editor: cannot move the asset to {}: {}", to, msg.view());
            return;
        }
        model_.refresh(session.project());
        model_.set_folder(asset_folder(to));
        seen_assets_ = session.project().assets().size();
    }

    AssetBrowserModel model_;
    usize seen_assets_ = ~usize(0);
    u64 seen_generation_ = ~u64(0);
    char filter_[64] = {};
    int type_ = 0;
    asset::Guid selected_, renaming_;
    char rename_buf_[256] = {};
    bool focus_rename_ = false;
};

}  // namespace

void add_asset_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<AssetPanel>(mem::heap())); }

}  // namespace mce::editor
