#pragma once
// The 2D translate gizmo (editor-tools.md §2.7; ADR-0025: hand-rolled, no gizmo library). Pure
// math + state over EditorScene, so dragging, axis constraints, snapping and cancel run headless
// in CI; the viewport only feeds it mouse positions and draws its lines.
//
// Handles are sized in screen pixels (constant on screen at every zoom): an X arrow, a Y arrow
// and a centre square for free XY movement. Dragging a selected entity's body is the same XY move.

#include "mce/editor/preview_renderer.h"

namespace mce::editor {

enum class GizmoPart : u8 { None, X, Y, XY };

// Snaps `v` to multiples of `step` (step <= 0: unchanged).
f32 snap_value(f32 v, f32 step);

class TranslateGizmo {
public:
    static constexpr f32 kArrowPx = 72.0f;  // arrow length
    static constexpr f32 kHeadPx = 11.0f;   // arrow head size
    static constexpr f32 kBoxPx = 16.0f;    // centre square side
    static constexpr f32 kPickPx = 7.0f;    // hit distance around an arrow shaft

    // The pivot: the selection anchor's position. False when nothing movable is selected.
    static bool pivot(const EditorScene& scene, math::vec2& out);
    // Which handle is under `mouse_px` (viewport pixels) for a gizmo at `pivot_world`.
    static GizmoPart hit(const EditorCamera2D& cam, rhi::Extent2D size, math::vec2 pivot_world, math::vec2 mouse_px);
    // Appends the gizmo's lines; `hot` is hovered, the active drag part is drawn highlighted.
    void lines(const EditorCamera2D& cam, math::vec2 pivot_world, GizmoPart hot, Array<OverlayLine>& out) const;

    // ---- drag ----
    // Starts moving every selected entity that has a LocalTransform.
    bool begin(const EditorScene& scene, GizmoPart part, math::vec2 mouse_world);
    // Moves the selection by (mouse - start) constrained to the part's axis. With `snap > 0` the
    // anchor's new position lands on the grid and the others keep their offsets to it.
    void update(EditorScene& scene, math::vec2 mouse_world, f32 snap);
    // Restores the positions captured at begin() (Esc / right click during a drag).
    void cancel(EditorScene& scene);
    void end() { part_ = GizmoPart::None; }
    bool dragging() const { return part_ != GizmoPart::None; }
    GizmoPart part() const { return part_; }
    // Total world offset of the anchor in the current drag (for the viewport readout).
    math::vec2 delta() const { return delta_; }

private:
    struct Start {
        EntityId id;
        math::vec3 position;
    };
    Array<Start> starts_;
    math::vec2 grab_;   // mouse world position at begin
    math::vec2 delta_;
    GizmoPart part_ = GizmoPart::None;
};

}  // namespace mce::editor
