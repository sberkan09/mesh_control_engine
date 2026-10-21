#pragma once
// The 2D transform gizmo: move, rotate and scale (editor-tools.md §2.7; ADR-0025: hand-rolled, no
// gizmo library). Pure math + state over EditorScene, so dragging, axis constraints, snapping and
// cancel run headless in CI; the viewport only feeds it mouse positions and draws its lines.
//
// Handles are sized in screen pixels (constant on screen at every zoom).
//   Move   an X arrow, a Y arrow and a centre square for free XY movement. Dragging a selected
//          entity's body is the same XY move.
//   Rotate a ring around the pivot; the drag angle turns every selected entity about the pivot
//          (snap: 15° steps).
//   Scale  X and Y handles ending in squares, and a centre square for uniform scale. Each entity's
//          scale is multiplied and its offset from the pivot scaled with it (snap: 0.1 steps).
// The pivot is the selection anchor; a multi-selection moves, turns and scales as one group.

#include "mce/editor/preview_renderer.h"

namespace mce::editor {

enum class GizmoPart : u8 { None, X, Y, XY, Ring };
enum class GizmoMode : u8 { Move, Rotate, Scale };
const char* gizmo_mode_label(GizmoMode mode);  // "Move", "Rotate", "Scale" (also the undo label)

// Snaps `v` to multiples of `step` (step <= 0: unchanged).
f32 snap_value(f32 v, f32 step);

class TransformGizmo {
public:
    static constexpr f32 kArrowPx = 72.0f;  // arrow length
    static constexpr f32 kHeadPx = 11.0f;   // arrow head size
    static constexpr f32 kBoxPx = 16.0f;    // centre square side
    static constexpr f32 kPickPx = 7.0f;    // hit distance around an arrow shaft
    static constexpr f32 kRingPx = 64.0f;   // rotate ring radius
    static constexpr f32 kRotateSnap = 0.2617994f;  // 15 degrees
    static constexpr f32 kScaleSnap = 0.1f;
    static constexpr f32 kMinScale = 0.01f;  // a drag through the pivot never flips or zeroes a scale

    void set_mode(GizmoMode mode) {
        if (!dragging()) mode_ = mode;
    }
    GizmoMode mode() const { return mode_; }

    // The pivot: the selection anchor's position in the scene. False when nothing movable is
    // selected. Children (ChildOf) are dragged in scene space and written back parent-relative;
    // a selected entity whose ancestor is also selected follows that ancestor instead.
    static bool pivot(const EditorScene& scene, math::vec2& out);
    // Which handle is under `mouse_px` (viewport pixels) for a gizmo at `pivot_world`.
    static GizmoPart hit(const EditorCamera2D& cam, rhi::Extent2D size, math::vec2 pivot_world, math::vec2 mouse_px,
                         GizmoMode mode = GizmoMode::Move);
    // Appends the gizmo's lines; `hot` is hovered, the active drag part is drawn highlighted.
    void lines(const EditorCamera2D& cam, math::vec2 pivot_world, GizmoPart hot, Array<OverlayLine>& out) const;

    // ---- drag ----
    // Starts transforming every selected entity that has a LocalTransform, in the current mode.
    bool begin(const EditorScene& scene, GizmoPart part, math::vec2 mouse_world);
    // Move: by (mouse - start) constrained to the part's axis; with `snap > 0` the anchor's new
    // position lands on the grid and the others keep their offsets to it. Rotate: by the angle the
    // mouse swept around the pivot (snap: kRotateSnap steps whenever `snap > 0`). Scale: by the
    // ratio of the mouse's distance from the pivot along the part's axis (snap: kScaleSnap steps).
    void update(EditorScene& scene, math::vec2 mouse_world, f32 snap);
    // Restores the positions captured at begin() (Esc / right click during a drag).
    void cancel(EditorScene& scene);
    void end() { part_ = GizmoPart::None; }
    bool dragging() const { return part_ != GizmoPart::None; }
    GizmoPart part() const { return part_; }
    // Total world offset of the anchor in the current drag (for the viewport readout).
    math::vec2 delta() const { return delta_; }
    // Rotate: the angle swept so far (radians). Scale: the factors applied so far.
    f32 angle() const { return angle_; }
    math::vec2 factor() const { return factor_; }

private:
    struct Start {
        EntityId id;
        math::vec3 position;  // in the scene (the composed WorldTransform), what the drag edits
        math::quat rotation;
        math::vec3 scale;
        LocalTransform local;  // as authored, for cancel
    };
    void place(EditorScene& scene, const Start& st, math::vec3 position, math::quat rotation, math::vec3 scale) const;
    Array<Start> starts_;
    math::vec2 grab_;   // mouse world position at begin
    math::vec2 pivot_;  // the anchor's position at begin
    math::vec2 delta_;
    f32 angle_ = 0.0f;
    math::vec2 factor_{1.0f, 1.0f};
    GizmoPart part_ = GizmoPart::None;
    GizmoMode mode_ = GizmoMode::Move;
};

using TranslateGizmo = TransformGizmo;  // the M4 name: the Move mode

}  // namespace mce::editor
