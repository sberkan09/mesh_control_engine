// The Terrain panel (M-TERRAIN 4): picks the brush for the selected entity's Terrain3D and toggles the
// orbit view's sculpt tool. Left drag on the terrain applies the brush (Ctrl: the opposite op, Alt:
// orbit instead); every stroke saves the .mcterrain and re-bakes it, and the panel undoes and redoes
// whole strokes.

#include <imgui.h>

#include <algorithm>

#include "mce/editor/editor_shell.h"
#include "mce/kit/kit3d.h"
#include "scene_panels.h"

namespace mce::editor {
namespace {

struct OpInfo {
    TerrainBrushOp op;
    const char* name;
    const char* strength;  // what the strength slider means
    f32 max;
};
constexpr OpInfo kOps[] = {
    {TerrainBrushOp::Raise, "Raise", "metres / s", 20.0f},  {TerrainBrushOp::Lower, "Lower", "metres / s", 20.0f},
    {TerrainBrushOp::Smooth, "Smooth", "pull / s", 10.0f},  {TerrainBrushOp::Flatten, "Flatten", "pull / s", 10.0f},
    {TerrainBrushOp::Noise, "Noise", "metres / s", 20.0f},  {TerrainBrushOp::Paint, "Paint", "weight / s", 10.0f},
    {TerrainBrushOp::Hole, "Hole", nullptr, 0.0f},          {TerrainBrushOp::Fill, "Fill hole", nullptr, 0.0f},
};

class TerrainPanel final : public IEditorPanel {
public:
    const char* name() const override { return "Terrain"; }

    void draw(EditorShell& shell) override {
        EditorSession& session = shell.session();
        EditorScene& scene = shell.scene();
        if (!session.project().is_open()) {
            ImGui::TextDisabled("Open a project to sculpt terrains.");
            return;
        }
        if (scene.selection().items().size() != 1) {
            ImGui::TextDisabled("Select one entity with a Terrain3D to sculpt it.");
            return;
        }
        if (shell.playing()) {
            ImGui::TextDisabled("Stop play to sculpt.");
            return;
        }
        TerrainEditor& te = shell.terrain();
        if (!te.bind(session, scene.selection().items()[0])) {
            ImGui::TextWrapped("The selected entity has no Terrain3D with a cooked .mcterrain. Add one in the Inspector "
                               "(an asset made with Create > Terrain or asset_create).");
            return;
        }
        const TerrainDocument& d = te.doc();
        const ProjectAsset* a = session.project().find(te.terrain());
        ImGui::Text("%s", a ? a->path.c_str() : "terrain");
        ImGui::TextDisabled("%u x %u samples over %.0f x %.0f m, heights %.1f to %.1f m, %u layers", d.resolution, d.resolution,
                            static_cast<f64>(d.size.x), static_cast<f64>(d.size.y), static_cast<f64>(d.height_min),
                            static_cast<f64>(d.height_max), d.layer_count);
        if (ImGui::Checkbox("Sculpt in the viewport", &shell.view().terrain_sculpt) && shell.view().terrain_sculpt)
            shell.view().orbit_view = true;
        ImGui::SameLine();
        ImGui::BeginDisabled(!te.can_undo() || te.in_stroke());
        if (ImGui::Button("Undo stroke")) te.undo(session);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!te.can_redo() || te.in_stroke());
        if (ImGui::Button("Redo stroke")) te.redo(session);
        ImGui::EndDisabled();
        ImGui::TextDisabled("Left drag on the terrain applies the brush; Ctrl the opposite; Alt+drag orbits.");

        ImGui::SeparatorText("Brush");
        TerrainBrush& b = te.brush;
        for (u32 i = 0; i < sizeof(kOps) / sizeof(kOps[0]); ++i) {
            if (i % 4 != 0) ImGui::SameLine();
            if (ImGui::RadioButton(kOps[i].name, b.op == kOps[i].op)) b.op = kOps[i].op;
        }
        const OpInfo* info = &kOps[0];
        for (const OpInfo& o : kOps)
            if (o.op == b.op) info = &o;
        ImGui::DragFloat("Radius", &b.radius, 0.1f, 0.25f, 0.5f * std::max(d.size.x, d.size.y), "%.2f m");
        if (info->strength) ImGui::DragFloat("Strength", &b.strength, 0.01f, 0.0f, info->max, info->strength);
        if (info->strength) ImGui::SliderFloat("Hardness", &b.hardness, 0.0f, 1.0f, "%.2f");
        if (b.op == TerrainBrushOp::Flatten)
            ImGui::TextDisabled("Flattens to the height where the stroke starts.");
        if (b.op == TerrainBrushOp::Noise) {
            int seed = static_cast<int>(b.seed);
            if (ImGui::DragInt("Seed", &seed, 0.2f, 0, 1 << 20)) b.seed = static_cast<u32>(seed);
        }
        if (b.op == TerrainBrushOp::Paint) {
            if (d.layer_count == 0) {
                ImGui::TextDisabled("This terrain has no splat layers (\"layers\" in the .mcterrain).");
            } else {
                int layer = static_cast<int>(b.layer < d.layer_count ? b.layer : 0);
                if (ImGui::SliderInt("Layer", &layer, 0, static_cast<int>(d.layer_count) - 1)) b.layer = static_cast<u32>(layer);
            }
        }
        const HotReloadResult& last = session.last_hot_reload();
        if (last.seconds > 0.0) ImGui::TextDisabled("Last re-bake: %.1f ms", last.seconds * 1000.0);
    }
};

}  // namespace

void add_terrain_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<TerrainPanel>(mem::heap())); }

}  // namespace mce::editor
