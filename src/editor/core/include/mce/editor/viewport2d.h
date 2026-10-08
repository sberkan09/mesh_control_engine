#pragma once
// 2D viewport math (editor-tools.md §2.10, §3.4): the editor camera, entity bounds and CPU picking.
// Deterministic, no GPU readback: an entity's quad is the sprite rectangle the batch draws (or a
// text/collider box, or a small marker), and picking walks quads top-most first.
//
// Screen space is viewport pixels, origin top-left, +y down; world space is metres, +y up.

#include "mce/editor/editor_scene.h"
#include "mce/math/math.h"
#include "mce/render/camera2d.h"

namespace mce::editor {

struct EditorCamera2D {
    math::vec2 center;
    f32 zoom = 1.0f;               // multiplies pixels_per_meter
    f32 pixels_per_meter = 48.0f;  // the art density sprites are authored at

    f32 ppm() const { return pixels_per_meter * zoom; }
    math::vec2 screen_to_world(math::vec2 px, rhi::Extent2D size) const;
    math::vec2 world_to_screen(math::vec2 w, rhi::Extent2D size) const;
    // Drag-pan: the world point under the cursor follows it.
    void pan_pixels(math::vec2 delta_px);
    // Zooms by `factor` keeping the world point under `px` fixed; zoom stays in [1/64, 64].
    void zoom_at(math::vec2 px, rhi::Extent2D size, f32 factor);
    // Frames a world rectangle (with a margin) in a viewport of `size`.
    void frame(const math::aabb2& box, rhi::Extent2D size);
    render::Camera2D to_render() const;  // no pixel snap: editing wants smooth zoom
};

// The entity's world quad, counter-clockwise from bottom-left. False when it has no LocalTransform.
bool entity_quad(const EditorScene& scene, EntityId id, math::vec2 out[4]);
bool point_in_quad(const math::vec2 q[4], math::vec2 p);
// The top-most entity whose quad contains `world` (sprite layer, then order, then file order —
// the draw order), or kNoEntity.
EntityId pick_entity(const EditorScene& scene, math::vec2 world);
// Bounds of every entity quad (empty scene: a 10 m box around the origin).
math::aabb2 scene_bounds(const EditorScene& scene);

}  // namespace mce::editor
