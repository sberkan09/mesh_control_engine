// The Layout panel (M-LAYOUTPANEL): every layout command in one window, for the entities selected in the open scene.
// Align, distribute, snap and flip; copies in a grid or a ring; turn and resize as one body; random variation; and a
// live read-out of where the selection sits. Each button is the same headless command the Edit menu and the MCP tools
// run (mce/editor/*_entities.h), so one click is one undo entry. The Edit menu keeps its items; this panel is the
// place to stay in while laying a level out, with the numbers kept between clicks.

#include <imgui.h>

#include <algorithm>

#include "mce/editor/align_entities.h"
#include "mce/editor/array_duplicate.h"
#include "mce/editor/editor_shell.h"
#include "mce/editor/flip_entities.h"
#include "mce/editor/group_bounds.h"
#include "mce/editor/jitter_entities.h"
#include "mce/editor/radial_duplicate.h"
#include "mce/editor/snap_entities.h"
#include "mce/editor/transform_group.h"
#include "scene_panels.h"

namespace mce::editor {
namespace {

class LayoutPanel final : public IEditorPanel {
public:
    LayoutPanel() { open = false; }  // the Window menu

    const char* name() const override { return kLayoutPanel; }

    void draw(EditorShell& shell) override {
        EditorScene& scene = shell.session().scene();
        Array<EntityId> ids;  // a copy: the commands below change the scene
        for (EntityId id : scene.selection().items()) ids.push_back(id);
        const Span<const EntityId> sel(ids.data(), ids.size());
        if (ids.empty()) {
            ImGui::TextDisabled("Select entities in the Hierarchy or the viewport.");
            return;
        }
        draw_measure(scene, sel);
        if (ImGui::CollapsingHeader("Line up", ImGuiTreeNodeFlags_DefaultOpen)) draw_line_up(scene, sel);
        if (ImGui::CollapsingHeader("Copies", ImGuiTreeNodeFlags_DefaultOpen)) draw_copies(scene, sel);
        if (ImGui::CollapsingHeader("Turn and resize", ImGuiTreeNodeFlags_DefaultOpen)) draw_transform(scene, sel);
        if (ImGui::CollapsingHeader("Randomize")) draw_randomize(scene, sel);
    }

private:
    static void measure_text(const char* label, f32 a, f32 b) { ImGui::Text("%s  %.2f, %.2f", label, a, b); }

    static void draw_measure(EditorScene& scene, Span<const EntityId> sel) {
        const GroupBounds g = group_bounds(scene, sel);
        ImGui::Text("%zu selected", static_cast<size_t>(sel.size()));
        if (g.count == 0) return;
        measure_text("Middle", g.center_x, g.center_y);
        measure_text("Size  ", g.width, g.height);
        ImGui::Text("Spread %.2f", g.diagonal);
        ImGui::Separator();
    }

    void draw_line_up(EditorScene& scene, Span<const EntityId> sel) {
        const bool two = sel.size() >= 2;
        ImGui::BeginDisabled(!two);
        if (ImGui::Button("Left")) align_entities(scene, sel, AlignAxis::X, AlignMode::Min);
        ImGui::SameLine();
        if (ImGui::Button("Middle X")) align_entities(scene, sel, AlignAxis::X, AlignMode::Center);
        ImGui::SameLine();
        if (ImGui::Button("Right")) align_entities(scene, sel, AlignAxis::X, AlignMode::Max);
        if (ImGui::Button("Bottom")) align_entities(scene, sel, AlignAxis::Y, AlignMode::Min);
        ImGui::SameLine();
        if (ImGui::Button("Middle Y")) align_entities(scene, sel, AlignAxis::Y, AlignMode::Center);
        ImGui::SameLine();
        if (ImGui::Button("Top")) align_entities(scene, sel, AlignAxis::Y, AlignMode::Max);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(sel.size() < 3);
        if (ImGui::Button("Space evenly X")) distribute_entities(scene, sel, AlignAxis::X);
        ImGui::SameLine();
        if (ImGui::Button("Space evenly Y")) distribute_entities(scene, sel, AlignAxis::Y);
        ImGui::EndDisabled();
        if (ImGui::Button("Flip X")) flip_entities(scene, sel, AlignAxis::X);
        ImGui::SameLine();
        if (ImGui::Button("Flip Y")) flip_entities(scene, sel, AlignAxis::Y);
        const f32 w = ImGui::GetFontSize() * 6.0f;
        ImGui::SetNextItemWidth(w);
        ImGui::DragFloat("##snap", &snap_step_, 0.01f, 0.001f, 1000.0f, "%.3f m");
        ImGui::SameLine();
        if (ImGui::Button("Snap to grid")) snap_entities(scene, sel, snap_step_);
    }

    void draw_copies(EditorScene& scene, Span<const EntityId> sel) {
        const f32 w = ImGui::GetFontSize() * 6.0f;
        ImGui::TextUnformatted("Grid");
        ImGui::SetNextItemWidth(w);
        ImGui::InputInt("Columns", &columns_);
        ImGui::SetNextItemWidth(w);
        ImGui::InputInt("Rows", &rows_);
        ImGui::SetNextItemWidth(w);
        ImGui::DragFloat("Step X", &dx_, 0.05f, -1000000.0f, 1000000.0f, "%.2f m");
        ImGui::SetNextItemWidth(w);
        ImGui::DragFloat("Step Y", &dy_, 0.05f, -1000000.0f, 1000000.0f, "%.2f m");
        columns_ = std::clamp(columns_, 1, 4096);
        rows_ = std::clamp(rows_, 1, 4096);
        ImGui::BeginDisabled(columns_ * rows_ < 2);
        if (ImGui::Button("Copy into a grid"))
            array_duplicate(scene, sel, static_cast<u32>(columns_), static_cast<u32>(rows_), dx_, dy_);
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::TextUnformatted("Ring");
        i32 count = static_cast<i32>(radial_.count);
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputInt("Groups in all", &count))
            radial_.count = static_cast<u32>(std::clamp<i32>(count, 2, static_cast<i32>(kMaxRadialCopies) + 1));
        ImGui::SetNextItemWidth(w);
        ImGui::DragFloat("Centre X", &radial_.center_x, 0.05f, -1000000.0f, 1000000.0f, "%.2f m");
        ImGui::SetNextItemWidth(w);
        ImGui::DragFloat("Centre Y", &radial_.center_y, 0.05f, -1000000.0f, 1000000.0f, "%.2f m");
        ImGui::SetNextItemWidth(w);
        ImGui::DragFloat("Span", &radial_.total_degrees, 0.5f, 1.0f, 3600.0f, "%.1f deg");
        ImGui::Checkbox("Turn the copies", &radial_.turn);
        ImGui::BeginDisabled(!radial_params_valid(radial_));
        if (ImGui::Button("Copy around the centre")) radial_duplicate(scene, sel, radial_);
        ImGui::EndDisabled();
    }

    void draw_transform(EditorScene& scene, Span<const EntityId> sel) {
        const f32 w = ImGui::GetFontSize() * 6.0f;
        ImGui::SetNextItemWidth(w);
        ImGui::DragFloat("Turn", &xform_.degrees, 0.5f, -3600.0f, 3600.0f, "%.1f deg");
        ImGui::SetNextItemWidth(w);
        ImGui::DragFloat("Size", &xform_.scale, 0.01f, 0.001f, 1000.0f, "x%.2f");
        ImGui::Checkbox("Pivot at a point (else the middle)", &xform_.use_pivot);
        ImGui::BeginDisabled(!xform_.use_pivot);
        ImGui::SetNextItemWidth(w);
        ImGui::DragFloat("Pivot X", &xform_.pivot_x, 0.05f, -1000000.0f, 1000000.0f, "%.2f m");
        ImGui::SetNextItemWidth(w);
        ImGui::DragFloat("Pivot Y", &xform_.pivot_y, 0.05f, -1000000.0f, 1000000.0f, "%.2f m");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!group_transform_valid(xform_));
        if (ImGui::Button("Apply turn and size")) transform_group(scene, sel, xform_);
        ImGui::EndDisabled();
    }

    void draw_randomize(EditorScene& scene, Span<const EntityId> sel) {
        JitterParams& j = jitter_;
        const f32 w = ImGui::GetFontSize() * 6.0f;
        ImGui::SetNextItemWidth(w);
        ImGui::DragFloat("Move X up to", &j.move_x, 0.05f, 0.0f, 1000.0f, "%.2f m");
        ImGui::SetNextItemWidth(w);
        ImGui::DragFloat("Move Y up to", &j.move_y, 0.05f, 0.0f, 1000.0f, "%.2f m");
        ImGui::SetNextItemWidth(w);
        ImGui::DragFloat("Turn up to", &j.rotate_degrees, 0.5f, 0.0f, 360.0f, "%.1f deg");
        ImGui::SetNextItemWidth(w);
        ImGui::DragFloat("Size from", &j.scale_min, 0.01f, 0.01f, 100.0f, "x%.2f");
        ImGui::SetNextItemWidth(w);
        ImGui::DragFloat("Size to", &j.scale_max, 0.01f, 0.01f, 100.0f, "x%.2f");
        j.scale_max = std::max(j.scale_max, j.scale_min);
        i32 seed = static_cast<i32>(j.seed);
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputInt("Seed", &seed)) j.seed = static_cast<u32>(std::max(seed, 0));
        ImGui::SameLine();
        if (ImGui::Button("New seed")) j.seed = (j.seed * 1664525u + 1013904223u) & 0x7FFFFFFFu;
        ImGui::BeginDisabled(!jitter_params_valid(j));
        if (ImGui::Button("Randomize")) jitter_entities(scene, sel, j);
        ImGui::EndDisabled();
    }

    f32 snap_step_ = 0.5f;
    i32 columns_ = 3, rows_ = 1;
    f32 dx_ = 1.0f, dy_ = 1.0f;
    RadialParams radial_;
    GroupTransformParams xform_{15.0f, 1.0f, false, 0.0f, 0.0f};
    JitterParams jitter_{0.5f, 0.5f, 15.0f, 0.9f, 1.1f, 1};
};

}  // namespace

void add_layout_panel(EditorShell& shell) { shell.add_panel(mem::make_unique<LayoutPanel>(mem::heap())); }

}  // namespace mce::editor
