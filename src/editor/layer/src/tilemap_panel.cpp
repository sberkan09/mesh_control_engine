// The Tilemap panel (ROADMAP M6, FR-EDT-120 / FR-R2D-115, ADR-0030 §8): gives the selected entity a
// new .mctilemap over a tileset texture, picks the brush from the tileset's cells, and toggles the
// viewport's paint tool (T): left drag paints, Ctrl erases, Shift+drag fills a rectangle. Every
// stroke saves the asset and hot-reloads it, so the viewport shows the re-baked layer at once.

#include <imgui.h>

#include <cstdio>

#include "mce/editor/editor_shell.h"
#include "mce/kit/components.h"
#include "mce/log.h"
#include "scene_panels.h"

namespace mce::editor {
namespace {

class TilemapPanel final : public IEditorPanel {
public:
    const char* name() const override { return "Tilemap"; }

    void draw(EditorShell& shell) override {
        EditorSession& session = shell.session();
        EditorScene& scene = shell.scene();
        if (!session.project().is_open()) {
            ImGui::TextDisabled("Open a project to paint tilemaps.");
            return;
        }
        if (scene.selection().items().size() != 1) {
            ImGui::TextDisabled("Select one entity to paint its tilemap.");
            return;
        }
        const EntityId id = scene.selection().items()[0];
        if (shell.playing()) {
            ImGui::TextDisabled("Stop play to paint.");
            return;
        }
        TilemapEditor& tm = shell.tilemap();
        if (!tm.bind(session, id)) {
            draw_create(shell, id);
            return;
        }
        draw_editor(shell, tm);
    }

private:
    void draw_create(EditorShell& shell, EntityId id) {
        EditorSession& session = shell.session();
        const DocEntity* e = shell.scene().doc().find(id);
        if (!e) return;
        if (id != form_for_) {  // a fresh form per entity
            form_for_ = id;
            std::snprintf(path_, sizeof(path_), "tilemaps/%s.mctilemap", e->name.c_str());
            error_[0] = 0;
        }
        ImGui::TextWrapped("%s has no tilemap. Create one:", e->name.c_str());
        ImGui::InputText("File", path_, sizeof(path_));

        // The tileset: any texture in the project.
        const ProjectAsset* tileset = nullptr;
        for (const ProjectAsset& a : session.project().assets())
            if (a.type == asset::ChunkType::Texture && a.guid == tileset_) tileset = &a;
        if (ImGui::BeginCombo("Tileset", tileset ? tileset->path.c_str() : "(choose a texture)")) {
            for (const ProjectAsset& a : session.project().assets()) {
                if (a.type != asset::ChunkType::Texture) continue;
                if (ImGui::Selectable(a.path.c_str(), a.guid == tileset_)) tileset_ = a.guid;
            }
            ImGui::EndCombo();
        }
        ImGui::DragInt2("Cells (cols, rows)", cells_, 0.1f, 1, 256);
        ImGui::DragInt2("Size (tiles)", size_, 0.2f, 1, 4096);
        ImGui::DragFloat("Tile size", &tile_size_, 0.05f, 0.05f, 64.0f, "%.2f m");
        ImGui::BeginDisabled(!tileset);
        if (ImGui::Button("Create tilemap")) {
            Result<asset::Guid> r = TilemapEditor::create(session, id, path_, tileset_, static_cast<u32>(cells_[0]),
                                                          static_cast<u32>(cells_[1]), static_cast<u32>(size_[0]),
                                                          static_cast<u32>(size_[1]), tile_size_);
            if (r) {
                error_[0] = 0;
                shell.view().tile_paint = true;
            } else {
                FixedString<512> msg;
                format_error(r.error(), msg);
                std::snprintf(error_, sizeof(error_), "%s", msg.c_str());
                MCE_LOG_ERROR("editor: create tilemap: {}", msg.view());
            }
        }
        ImGui::EndDisabled();
        if (error_[0]) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", error_);
    }

    void draw_editor(EditorShell& shell, TilemapEditor& tm) {
        EditorSession& session = shell.session();
        const TilemapDocument& d = tm.doc();
        const ProjectAsset* map = session.project().find(tm.map());
        ImGui::Text("%s", map ? map->path.c_str() : d.name.c_str());
        ImGui::TextDisabled("%u x %u tiles, %.2f m, tileset %u x %u", d.width, d.height, static_cast<f64>(d.tile_size),
                            d.columns, d.rows);
        ImGui::Checkbox("Paint (T)", &shell.view().tile_paint);
        ImGui::SameLine();
        ImGui::BeginDisabled(!tm.can_undo());
        if (ImGui::Button("Undo stroke")) tm.undo(session);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!tm.can_redo());
        if (ImGui::Button("Redo stroke")) tm.redo(session);
        ImGui::EndDisabled();
        ImGui::TextDisabled("Left drag paints, Ctrl erases, Shift+drag fills a rectangle.");
        const HotReloadResult& last = session.last_hot_reload();
        if (last.seconds > 0.0) ImGui::TextDisabled("Last re-bake: %.1f ms", last.seconds * 1000.0);

        // The brush: 0 erases, n is the tileset's cell n-1 (row-major from the top-left).
        ImGui::SeparatorText("Brush");
        const f32 cell = ImGui::GetFrameHeight() * 1.4f;
        const u32 per_row = d.columns > 0 ? d.columns : 1;
        if (ImGui::BeginChild("##palette", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
            if (ImGui::Selectable("Erase", tm.brush == 0, ImGuiSelectableFlags_None, ImVec2(cell * 2.0f, cell))) tm.brush = 0;
            for (u32 i = 0; i < d.cells(); ++i) {
                if (i % per_row != 0) ImGui::SameLine();
                ImGui::PushID(static_cast<int>(i));
                char label[24];
                std::snprintf(label, sizeof(label), "%u", i + 1);
                if (ImGui::Selectable(label, tm.brush == i + 1, ImGuiSelectableFlags_None, ImVec2(cell, cell))) tm.brush = i + 1;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("cell %u: column %u, row %u from the top", i + 1, i % per_row, i / per_row);
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }

    EntityId form_for_ = kNoEntity;
    char path_[256] = {};
    char error_[512] = {};
    asset::Guid tileset_;
    int cells_[2] = {4, 4};
    int size_[2] = {32, 18};
    f32 tile_size_ = 1.0f;
};

}  // namespace

void add_tilemap_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<TilemapPanel>(mem::heap())); }

}  // namespace mce::editor
