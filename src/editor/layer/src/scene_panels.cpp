// The scene-editing panels (editor-tools.md §2.5, §2.6, FR-EDT-070): Hierarchy, Inspector, Cook.
// Every edit goes through EditorScene (the mutation funnel); these panels only draw and dispatch.

#include <imgui.h>

#include <cstdio>
#include <cstring>

#include "imgui_draw_ctx.h"
#include "mce/ecs/transform.h"
#include "mce/editor/editor_shell.h"
#include "scene_panels.h"

namespace mce::editor {

void delete_selection(EditorScene& scene) {
    Array<EntityId> ids;
    for (EntityId id : scene.selection().items()) ids.push_back(id);
    scene.begin_edit("Delete");
    for (EntityId id : ids) scene.destroy_entity(id);
    scene.end_edit();
}

namespace {

constexpr const char* kEntityPayload = "MCE_ENTITY";

// ---- Hierarchy ---------------------------------------------------------------------------------
// The scene's entities as a ChildOf tree (siblings in file order): select, multi-select (Ctrl
// toggles, Shift selects a range), create, rename (F2 / double-click), delete (Del, with the
// subtree). Drag onto an entity to make it the parent (the child keeps its place in the scene),
// Shift-drag to put it before that entity among its siblings, drag below the tree to unparent.
class HierarchyPanel final : public IEditorPanel {
public:
    const char* name() const override { return "Hierarchy"; }

    void draw(EditorShell& shell) override {
        if (!shell.scene_mode()) {
            draw_open_project(shell);
            return;
        }
        EditorScene& scene = shell.scene();
        const SceneDocument& doc = scene.doc();
        if (shell.playing()) {  // selection still works (the Play panel inspects it); edits do not
            draw_locked(shell);
            return;
        }
        const f32 right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        const StringView scene_name = doc.name().empty() ? StringView("untitled") : doc.name();
        ImGui::Text("%.*s%s", static_cast<int>(scene_name.size()), scene_name.data(), scene.dirty() ? " *" : "");
        ImGui::SameLine(right - ImGui::CalcTextSize("+ Entity").x - ImGui::GetStyle().FramePadding.x * 2);
        if (ImGui::SmallButton("+ Entity")) create(shell, -1);
        ImGui::Separator();

        if (ImGui::BeginChild("##entities")) {
            drop_ = Drop{};
            Array<EntityId> roots;
            scene.children_of(kNoEntity, roots);
            for (EntityId id : roots) draw_node(shell, id, 0);
            if (drop_.id != kNoEntity) {
                if (drop_.reorder_before != kNoEntity) {
                    // Shift-drop: same parent as the target, placed before it.
                    scene.begin_edit("Reorder");
                    scene.set_parent(drop_.id, scene.parent_of(drop_.reorder_before));
                    const i32 from = doc.index_of(drop_.id), to = doc.index_of(drop_.reorder_before);
                    scene.move_entity(drop_.id, static_cast<usize>(from < to ? to - 1 : to));
                    scene.end_edit();
                } else {
                    scene.set_parent(drop_.id, drop_.parent);
                }
            }

            // Empty space: click clears the selection, right-click creates.
            ImGui::Dummy(ImGui::GetContentRegionAvail());
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) scene.selection().clear();
            if (ImGui::BeginDragDropTarget()) {  // dropped below the tree: becomes a root
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kEntityPayload)) {
                    EntityId id = kNoEntity;
                    std::memcpy(&id, p->Data, sizeof(id));
                    scene.set_parent(id, kNoEntity);
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem("##empty")) {
                if (ImGui::MenuItem("Create Entity")) create(shell, -1);
                ImGui::EndPopup();
            }

            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
                if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) delete_selection(scene);
                if (ImGui::IsKeyPressed(ImGuiKey_F2, false))
                    if (const DocEntity* a = doc.find(scene.selection().anchor())) begin_rename(*a);
            }
        }
        ImGui::EndChild();
    }

private:
    struct Drop {
        EntityId id = kNoEntity;              // the dragged entity
        EntityId parent = kNoEntity;          // its new parent
        EntityId reorder_before = kNoEntity;  // Shift-drop: its new next sibling
    };

    void draw_node(EditorShell& shell, EntityId id, usize depth) {
        EditorScene& scene = shell.scene();
        const DocEntity* ep = scene.doc().find(id);
        if (!ep || depth > scene.doc().size()) return;  // a ChildOf cycle draws once around
        const DocEntity& e = *ep;
        Array<EntityId> kids;
        scene.children_of(id, kids);
        ImGui::PushID(static_cast<int>(e.id));
        if (rename_ == e.id) {
            ImGui::Indent(ImGui::GetTreeNodeToLabelSpacing());
            draw_rename(scene, e);
            ImGui::Unindent(ImGui::GetTreeNodeToLabelSpacing());
        } else {
            const bool selected = scene.selection().contains(e.id);
            const char* label = e.name.view().empty() ? "(unnamed)" : e.name.c_str();
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                                       ImGuiTreeNodeFlags_DefaultOpen;
            if (kids.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (selected) flags |= ImGuiTreeNodeFlags_Selected;
            const bool expanded = ImGui::TreeNodeEx("##node", flags, "%s", label);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
                const ImGuiIO& io = ImGui::GetIO();
                if (io.KeyShift) scene.select_range(e.id, io.KeyCtrl);
                else if (io.KeyCtrl) scene.selection().toggle(e.id);
                else scene.selection().set(e.id);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) shell.frame_scene();
            }
            if (ImGui::BeginDragDropSource()) {
                const EntityId dragged = e.id;
                ImGui::SetDragDropPayload(kEntityPayload, &dragged, sizeof(dragged));
                ImGui::TextUnformatted(label);
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kEntityPayload)) {
                    std::memcpy(&drop_.id, p->Data, sizeof(drop_.id));
                    if (ImGui::GetIO().KeyShift) drop_.reorder_before = e.id;
                    else drop_.parent = e.id;
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem("##ctx")) {
                if (!scene.selection().contains(e.id)) scene.selection().set(e.id);
                if (ImGui::MenuItem("Rename", "F2")) begin_rename(e);
                if (ImGui::MenuItem("Delete", "Del")) delete_selection(scene);
                if (ImGui::MenuItem("Unparent", nullptr, false, scene.parent_of(e.id) != kNoEntity)) scene.set_parent(e.id, kNoEntity);
                ImGui::Separator();
                if (ImGui::MenuItem("Create Child")) create(shell, scene.doc().index_of(e.id) + 1, e.id);
                if (ImGui::MenuItem("Create Entity Below")) create(shell, scene.doc().index_of(e.id) + 1);
                ImGui::EndPopup();
            }
            if (expanded && !kids.empty()) {
                for (EntityId k : kids) draw_node(shell, k, depth + 1);
                ImGui::TreePop();
            }
        }
        ImGui::PopID();
    }

    void create(EditorShell& shell, i32 at, EntityId parent = kNoEntity) {
        EditorScene& scene = shell.scene();
        scene.begin_edit("Create entity");
        const EntityId id = scene.create_entity("Entity", true, at);
        // New entities appear where the user is looking (a child: on its parent).
        LocalTransform t;
        t.position = {shell.camera().center.x, shell.camera().center.y, 0.0f};
        if (parent != kNoEntity) t.position = scene.world_transform(parent).position;
        scene.set(id, t);
        if (parent != kNoEntity) scene.set_parent(id, parent);
        scene.end_edit();
        scene.selection().set(id);
        begin_rename(*scene.doc().find(id));
    }

    void draw_locked(EditorShell& shell) {
        EditorScene& scene = shell.scene();
        ImGui::TextDisabled("Playing: the scene is locked until Stop (F5).");
        ImGui::Separator();
        if (ImGui::BeginChild("##entities")) {
            for (const DocEntity& e : scene.doc().entities()) {
                ImGui::PushID(static_cast<int>(e.id));
                const char* label = e.name.view().empty() ? "(unnamed)" : e.name.c_str();
                if (ImGui::Selectable(label, scene.selection().contains(e.id))) scene.selection().set(e.id);
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }

    void begin_rename(const DocEntity& e) {
        rename_ = e.id;
        std::snprintf(rename_buf_, sizeof(rename_buf_), "%s", e.name.c_str());
        rename_focus_ = true;
    }

    void draw_rename(EditorScene& scene, const DocEntity& e) {
        if (rename_focus_) {
            ImGui::SetKeyboardFocusHere();
            rename_focus_ = false;
        }
        ImGui::SetNextItemWidth(-1.0f);
        const bool enter = ImGui::InputText("##rename", rename_buf_, sizeof(rename_buf_),
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (enter || ImGui::IsItemDeactivatedAfterEdit()) {
            scene.rename_entity(e.id, StringView(rename_buf_));
            rename_ = kNoEntity;
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                   (!ImGui::IsItemActive() && !ImGui::IsItemFocused() && !rename_focus_ && ImGui::IsMouseClicked(0))) {
            rename_ = kNoEntity;
        }
    }

    void draw_open_project(EditorShell& shell) {
        ImGui::TextWrapped("No project open. A project is a folder with an assets/ directory.");
        ImGui::SetNextItemWidth(-1.0f);
        const bool enter = ImGui::InputTextWithHint("##project", "project folder (e.g. samples/scene_demo)", project_buf_,
                                                    sizeof(project_buf_), ImGuiInputTextFlags_EnterReturnsTrue);
        if ((ImGui::Button("Open Project") || enter) && project_buf_[0]) {
            if (shell.open_project(StringView(project_buf_))) {
                Array<const ProjectAsset*> scenes;
                shell.session().list_scenes(scenes);
                if (!scenes.empty()) shell.open_scene(scenes[0]->path.view());
            }
        }
    }

    EntityId rename_ = kNoEntity;
    Drop drop_;
    char rename_buf_[64] = {};
    bool rename_focus_ = false;
    char project_buf_[512] = {};
};

// ---- Inspector ---------------------------------------------------------------------------------
class InspectorPanel final : public IEditorPanel {
public:
    const char* name() const override { return "Inspector"; }

    void draw(EditorShell& shell) override {
        if (!shell.scene_mode()) {
            ImGui::TextDisabled("Open a project to edit a scene.");
            return;
        }
        EditorScene& scene = shell.scene();
        const EntityId id = scene.selection().anchor();
        if (id == kNoEntity || !scene.doc().find(id)) {
            ImGui::TextDisabled("Select an entity in the Hierarchy or the Viewport.");
            return;
        }
        if (shell.playing()) {
            ImGui::TextDisabled("Playing: live values are in the Play panel; Stop (F5) to edit.");
            return;
        }
        if (scene.selection().items().size() > 1)
            ImGui::TextDisabled("%u selected; editing the first.", static_cast<u32>(scene.selection().items().size()));
        ImGuiDrawCtx ctx(shell.session(), filter_);
        ImGui::PushItemWidth(-ImGui::GetFontSize() * 7.0f);
        draw_inspector(ctx, scene, id);
        ImGui::PopItemWidth();
    }

private:
    char filter_[ImGuiDrawCtx::kFilterSize] = {};
};

// ---- Cook (FR-EDT-070) -----------------------------------------------------------------------------
// The last cook's result and every diagnostic as file:line:col, plus scene-open and catalog
// errors. Failed assets keep serving their previous output.
class CookPanel final : public IEditorPanel {
public:
    const char* name() const override { return "Cook"; }

    void draw(EditorShell& shell) override {
        if (!shell.scene_mode()) {
            ImGui::TextDisabled("Open a project to cook it.");
            return;
        }
        EditorProject& project = shell.session().project();
        if (ImGui::Button("Cook (F7)")) shell.cook(false);
        ImGui::SameLine();
        if (ImGui::Button("Cook + Pack")) shell.cook(true);
        ImGui::SameLine();
        const cook::CookReport& r = project.last_report();
        if (!project.has_cooked()) {
            ImGui::TextDisabled("not cooked yet");
        } else {
            const ImVec4 c = r.ok() ? ImVec4(0.5f, 0.9f, 0.5f, 1) : ImVec4(1, 0.45f, 0.4f, 1);
            ImGui::TextColored(c, "%s: %u cooked, %u up to date, %u failed, %u error(s), %u warning(s)",
                               r.ok() ? "OK" : "FAILED", r.cooked, r.up_to_date, r.failed, r.diags.error_count(),
                               r.diags.warning_count());
        }
        ImGui::Separator();
        if (ImGui::BeginChild("##diags", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
            u32 shown = 0;
            shown += list(project.scan_diags(), "catalog");
            shown += list(shell.session().scene_diags(), "scene");
            shown += list(r.diags, "cook");
            for (const cook::AssetReport& a : r.assets) {
                if (a.status != cook::AssetStatus::Failed && a.status != cook::AssetStatus::Blocked) continue;
                ImGui::TextColored(ImVec4(1, 0.7f, 0.4f, 1), "%s %s (previous output keeps serving)",
                                   a.status == cook::AssetStatus::Failed ? "failed: " : "blocked:", a.path.c_str());
                ++shown;
            }
            if (shown == 0) ImGui::TextDisabled("No problems.");
        }
        ImGui::EndChild();
    }

private:
    static u32 list(const cook::DiagList& diags, const char* source) {
        u32 n = 0;
        for (const cook::Diagnostic& d : diags.all()) {
            cook::String line;
            cook::format_diagnostic(d, line);
            const bool err = d.severity == cook::Severity::Error;
            ImGui::PushID(static_cast<int>(n));
            ImGui::TextColored(err ? ImVec4(1, 0.45f, 0.4f, 1) : ImVec4(1, 0.8f, 0.4f, 1), "[%s] %s", source, line.c_str());
            if (ImGui::BeginPopupContextItem("##copy")) {
                if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(line.c_str());
                ImGui::EndPopup();
            }
            ImGui::PopID();
            ++n;
        }
        return n;
    }
};

}  // namespace

void add_scene_panels(EditorShell& shell) {
    shell.add_panel(mem::make_unique<HierarchyPanel>(mem::heap()));
    shell.add_panel(mem::make_unique<InspectorPanel>(mem::heap()));
    shell.add_panel(mem::make_unique<CookPanel>(mem::heap()));
}

}  // namespace mce::editor
