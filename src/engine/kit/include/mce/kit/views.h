#pragma once
// Views (M-VIEWS): several cameras drawn in one frame. A CameraView on a camera entity (one with a
// Camera3DComponent or a Camera2DComponent) gives it a rect of a target: the window, or (M-VIEWS 3)
// a RenderTexture entity. collect_views() turns them into the frame's view list, which the player
// and the editor's play view draw in order.
//
//   Array<kit::ViewEntry> views(mem::frame());
//   if (kit::collect_views(world, backbuffer, views))   // false: no CameraView, one primary camera
//       for (const kit::ViewEntry& v : views) ...        // draw v.camera into v.rect of v.target
//
// Rules:
//   - Without any enabled CameraView the scene draws as before: the primary camera(s) full screen.
//   - With one, every camera that has an enabled CameraView is a view, and so is each primary
//     camera without one (full window, order 0): a minimap camera only needs its own CameraView.
//   - Order: views into textures first (they are drawn before the views that may show them), then
//     by `order` (lower first, so higher draws on top), a 3D camera before a 2D one of the same
//     order (a 2D HUD camera over a 3D world, as on the single-view path), then entity index.
//   - Rects are fractions of the target, top-left origin, rounded to whole pixels so neighbouring
//     views share their edges exactly.
//
// The view list is render state only: nothing here is read by the simulation, so the sim hash does
// not depend on how many views draw or how the screen is split.

#include "mce/containers/array.h"
#include "mce/ecs/world.h"
#include "mce/math/math.h"
#include "mce/refl/macros.h"
#include "mce/render/renderer.h"
#include "mce/render/scene3d.h"
#include "mce/rhi/types.h"

namespace mce::kit {

struct CameraView {
    math::vec4 viewport{0.0f, 0.0f, 1.0f, 1.0f};  // x, y, width, height as fractions of the target
    Entity target;                                // a RenderTexture entity; null = the window
    i32 order = 0;                                // lower draws first within its target
    u32 enabled = 1;
};
MCE_COMPONENT(CameraView, (viewport, Help{"x, y, width, height as fractions of the target, from its top-left corner"}),
              (target, Help{"a RenderTexture entity to draw into; empty: the window"}),
              (order, Help{"lower draws first (higher draws on top) within the target"}), enabled)

struct ViewEntry {
    Entity camera;
    Entity target;      // null = the window
    rhi::Rect2D rect;   // pixels of the target, top-left origin; never empty
    i32 order = 0;
    bool is_3d = false; // Camera3DComponent (else Camera2DComponent)
};

// A normalized rect as pixels of `size` (edges rounded, clamped to the target; may be empty).
rhi::Rect2D view_rect(const math::vec4& viewport, rhi::Extent2D size);

// The frame's views, sorted as above. `window` is the window's size in pixels; a view into a
// texture gets its rect from `target_size(target entity)` (null: such views are left out, as are
// views whose rect comes out empty). False, with `out` empty, when no enabled CameraView exists.
using TargetSizeFn = rhi::Extent2D (*)(World& world, Entity target, void* user);
bool collect_views(World& world, rhi::Extent2D window, Array<ViewEntry>& out, TargetSizeFn target_size = nullptr,
                   void* user = nullptr);

class Kit3DExtract;
class SpriteExtract;

// What record_views() draws with. `extract3d` null: 3D views are skipped (the host's 3D path is
// not running).
struct ViewRecordContext {
    render::Renderer* renderer = nullptr;
    Kit3DExtract* extract3d = nullptr;
    SpriteExtract* extract2d = nullptr;
    rhi::TextureHandle target;            // the window's: null = backbuffer, or the editor's play view
    rhi::Extent2D target_size;
    math::Color clear{0.08f, 0.09f, 0.12f, 1.0f};
    f32 alpha = 1.0f;
    f32 frame_dt = 1.0f / 60.0f;          // the frame's first 3D extract takes it (Kit3DExtract::set_frame_dt)
    render::LodSettings lod;
};
struct ViewRecordStats {
    u32 views_3d = 0;
    u32 views_2d = 0;
    render::Scene3D::Stats first_3d;       // the first 3D view's scene
    render::BatchStats first_2d;
};

// Records the window's views of `views` (collect_views): the target cleared once to ctx.clear,
// then each view in order. A 3D view is its own Scene3D (extract, then extract_view for the later
// ones) in its own Forward3D view slot (Renderer::view_slot, keyed by the camera entity); a 2D view
// is a sprite batch drawn with the pass's viewport on its rect, cleared to its camera's clear color
// unless a 3D view drawn before it covers its rect (a HUD camera over a 3D world). Outside any pass.
// The caller ends the frame's views with Renderer::end_views().
ViewRecordStats record_views(rhi::CommandBuffer& cmd, World& world, Span<const ViewEntry> views, const ViewRecordContext& ctx);

}  // namespace mce::kit
