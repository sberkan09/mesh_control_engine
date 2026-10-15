// The UI Document panel (ROADMAP M-UI-DATA step 6): a .mcui document's widget tree, the selected
// widget's inspector and a live preview at phone, tablet and desktop sizes, over the same assets the
// cook and the MCP ui_* tools read. Edits go through UiDocumentEditor (ui_document_editor.h): each
// is read back by the cook before it is kept, saved canonically when no widget is held any more and
// hot-reloaded; a change made on disk meanwhile (an MCP tool) is read in.

#include <imgui.h>

#include <algorithm>
#include <cstdio>

#include "imgui_draw_ctx.h"
#include "imgui_renderer.h"
#include "mce/editor/editor_shell.h"
#include "mce/editor/ui_document_editor.h"
#include "mce/log.h"
#include "scene_panels.h"

namespace mce::editor {
namespace {

class UiDocumentPanel final : public IEditorPanel {
public:
    UiDocumentPanel() { open = false; }  // the View menu, or a double click on a .mcui in Assets

    const char* name() const override { return kUiDocumentPanel; }

    void draw(EditorShell& shell) override {
        EditorSession& session = shell.session();
        UiDocumentEditor& ed = shell.ui_document();
        if (!session.project().is_open()) {
            ImGui::TextDisabled("Open a project to edit its UI documents.");
            return;
        }
        // A finished edit (no widget held) goes to disk; a change on disk comes in.
        if (ed.dirty() && !ImGui::IsAnyItemActive()) save(shell);
        const f64 now = ImGui::GetTime();
        if (now - synced_ > 0.5) {
            synced_ = now;
            ed.sync(session);
        }
        draw_header(shell, ed);
        if (!ed.is_open()) return;
        draw_toolbar(shell, ed);
        if (!ed.last_error().empty())
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%.*s", static_cast<int>(ed.last_error().size()),
                               ed.last_error().data());

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const f32 left = std::max(220.0f, avail.x * 0.42f);
        if (ImGui::BeginChild("##edit", ImVec2(left, 0), ImGuiChildFlags_ResizeX)) {
            draw_tree(ed);
            ImGui::SeparatorText("Inspector");
            ImGuiDrawCtx dc(session, filter_);
            draw_ui_inspector(dc, ed, state_, every_state_);
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##preview")) draw_preview(shell);
        ImGui::EndChild();
    }

    void record(EditorShell& shell, rhi::CommandBuffer& cmd, f32) override {
        if (!open || !visible_ || target_.is_null()) return;
        visible_ = false;  // the next draw says again
        const UiDocumentEditor& ed = shell.ui_document();
        if (!ed.is_open()) return;
        preview_.record(cmd, shell.preview(), ed, shell.session().hot_reload_count(), target_, target_size_, 1.0f,
                        pointer_, pointer_down_);
    }

    void shutdown(EditorShell& shell) override {
        if (shell.ui_document().dirty()) save(shell);
        preview_.reset();
        if (!target_.is_null()) shell.device().destroy_texture(target_);
        target_ = {};
    }

private:
    void save(EditorShell& shell) {
        if (Result<void> r = shell.ui_document().save(shell.session()); !r) {
            FixedString<512> msg;
            format_error(r.error(), msg);
            MCE_LOG_ERROR("editor: saving the UI document failed: {}", msg.view());
        }
    }

    void draw_header(EditorShell& shell, UiDocumentEditor& ed) {
        EditorSession& session = shell.session();
        const FixedString<200> label(ed.is_open() ? ed.path() : StringView("(choose a UI document)"));
        ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x * 0.42f));
        if (ImGui::BeginCombo("##document", label.c_str())) {
            for (const ProjectAsset& a : session.project().assets()) {
                if (a.type != asset::ChunkType::UiDocument) continue;
                if (ImGui::Selectable(a.path.c_str(), a.guid == ed.guid()) && a.guid != ed.guid()) {
                    if (ed.dirty()) save(shell);
                    ed.open(session, a.guid);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("New...")) ImGui::OpenPopup("##new");
        if (ImGui::BeginPopup("##new")) {
            ImGui::InputText("File", new_path_, sizeof(new_path_));
            ImGui::Checkbox("Modal (a menu; off: a HUD)", &new_modal_);
            if (ImGui::Button("Create")) {
                if (ed.dirty()) save(shell);
                if (Result<asset::Guid> r = ed.create(session, StringView(new_path_), new_modal_); r) {
                    ImGui::CloseCurrentPopup();
                    new_error_[0] = 0;
                } else {
                    FixedString<512> msg;
                    format_error(r.error(), msg);
                    std::snprintf(new_error_, sizeof(new_error_), "%s", msg.c_str());
                }
            }
            if (new_error_[0]) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", new_error_);
            ImGui::EndPopup();
        }
    }

    void draw_toolbar(EditorShell& shell, UiDocumentEditor& ed) {
        EditorSession& session = shell.session();
        ImGui::BeginDisabled(!ed.can_undo());
        if (ImGui::Button("Undo")) ed.undo(session);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!ed.can_redo());
        if (ImGui::Button("Redo")) ed.redo(session);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110.0f);
        const refl::EnumRecord& kinds = ui::mce_refl_enum_record_UiWidgetKind;
        const refl::EnumValue* cur = kinds.find(kind_);
        if (ImGui::BeginCombo("##kind", cur ? cur->name_str : "?")) {
            for (const refl::EnumValue& v : kinds.values)
                if (ImGui::Selectable(v.name_str, v.value == kind_)) kind_ = v.value;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        const u32 sel = ed.selected();
        if (ImGui::Button("Add")) {
            if (ed.add_widget(sel, static_cast<ui::WidgetKind>(kind_))) save(shell);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(sel == ui::kNoWidgetIndex ? "Add at the root"
                                                        : "Add as the selected widget's last child");
        ImGui::SameLine();
        ImGui::BeginDisabled(sel == ui::kNoWidgetIndex);
        if (ImGui::Button("Remove") && ed.remove_widget(sel)) save(shell);
        ImGui::SameLine();
        if (ImGui::ArrowButton("##up", ImGuiDir_Up) && ed.move_widget(sel, -1)) save(shell);
        ImGui::SameLine();
        if (ImGui::ArrowButton("##down", ImGuiDir_Down) && ed.move_widget(sel, 1)) save(shell);
        ImGui::EndDisabled();
    }

    void draw_tree(UiDocumentEditor& ed) {
        const ui::DocumentData& d = ed.data();
        if (ImGui::Selectable("(document)", ed.selected() == ui::kNoWidgetIndex)) ed.select(ui::kNoWidgetIndex);
        for (u32 i = 0; i < d.widgets.size(); ++i)
            if (d.widgets[i].parent == ui::kNoWidgetIndex) draw_node(ed, i);
    }

    void draw_node(UiDocumentEditor& ed, u32 i) {
        const ui::DocumentData& d = ed.data();
        const ui::WidgetData& w = d.widgets[i];
        const u32 end = ed.subtree_end(i);
        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (end == i + 1) flags |= ImGuiTreeNodeFlags_Leaf;
        if (ed.selected() == i) flags |= ImGuiTreeNodeFlags_Selected;
        const refl::EnumValue* k = ui::mce_refl_enum_record_UiWidgetKind.find(static_cast<i64>(w.kind));
        ImGui::PushID(static_cast<int>(i));
        const bool node_open = ImGui::TreeNodeEx("##w", flags, "%s  %s", w.name.c_str(), k ? k->name_str : "?");
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) ed.select(i);
        if (node_open) {
            for (u32 c = i + 1; c < end; ++c)
                if (d.widgets[c].parent == i) draw_node(ed, c);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    void draw_preview(EditorShell& shell) {
        const Span<const UiPreviewSize> sizes = ui_preview_sizes();
        if (size_ >= sizes.size()) size_ = 0;
        for (u32 i = 0; i < sizes.size(); ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(sizes[i].name, size_ == i)) size_ = i;
        }
        const rhi::Extent2D px = sizes[size_].size;
        ImGui::SameLine();
        ImGui::TextDisabled("%u x %u", px.width, px.height);
        if (target_.is_null() || !(px == target_size_)) {
            rhi::RenderDevice& device = shell.device();
            if (!target_.is_null()) device.destroy_texture(target_);
            rhi::TextureDesc td;
            td.size = px;
            td.format = rhi::TextureFormat::RGBA8_UNorm;
            td.usage = rhi::TextureUsage::SampledColorTarget;
            td.debug_name = "editor.ui_preview";
            target_ = device.create_texture(td);
            target_size_ = px;
            if (target_.is_null()) return;
        }
        // The whole screen, as large as the panel allows, its shape kept.
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const f32 s = std::min(avail.x / static_cast<f32>(px.width), avail.y / static_cast<f32>(px.height));
        if (s <= 0.0f) return;
        const ImVec2 shown(static_cast<f32>(px.width) * s, static_cast<f32>(px.height) * s);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Image(ImGuiRhiRenderer::to_id(target_), shown, ImVec2(0, 1), ImVec2(1, 0));
        visible_ = true;
        if (ImGui::IsItemHovered()) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            pointer_ = {(m.x - origin.x) / s, (m.y - origin.y) / s};
            pointer_down_ = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        } else {
            pointer_ = {-1, -1};
            pointer_down_ = false;
        }
    }

    UiDocumentPreview preview_;
    rhi::TextureHandle target_;
    rhi::Extent2D target_size_;
    u32 size_ = 0;
    bool visible_ = false;
    math::vec2 pointer_{-1, -1};
    bool pointer_down_ = false;
    u32 state_ = 0;
    bool every_state_ = true;
    i64 kind_ = static_cast<i64>(ui::WidgetKind::Button);
    f64 synced_ = 0.0;
    char filter_[ImGuiDrawCtx::kFilterSize] = {};
    char new_path_[256] = "ui/menu.mcui";
    bool new_modal_ = true;
    char new_error_[512] = {};
};

}  // namespace

void add_ui_document_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<UiDocumentPanel>(mem::heap())); }

}  // namespace mce::editor
