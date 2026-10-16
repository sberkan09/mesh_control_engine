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
//     order (a 2D HUD camera over a 3D world, as on the single-view path), then split player,
//     then entity index.
//   - Rects are fractions of the target, top-left origin, rounded to whole pixels so neighbouring
//     views share their edges exactly.
//   - Split-screen (M-VIEWS 4): a window view with `split` = n > 0 draws into the cell of player n.
//     The cells come from the number of distinct `split` values in use (players 1 and 3 present:
//     a two-way split, 1 left, 3 right) and the scene's SplitScreen layout; its `viewport` is then
//     a part of the cell (full by default), so a per-player HUD or minimap camera shares the
//     player's `split` with a higher `order` and a smaller viewport. A player leaving reflows the rest.
//   - Small views cost less (record_views, ViewRecordContext::scale_quality): a view covering at
//     most half the window draws two shadow cascades at most; one at most about a third (a three-
//     or four-way split cell, a picture-in-picture) also drops screen-space reflections, contact
//     shadows, volumetric light, depth of field and motion blur. A full-window view is untouched.
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
    u32 split = 0;  // 1..4: the split-screen cell of that player (the window only); 0 = none
    u32 pad_[3] = {};
};
MCE_COMPONENT(CameraView, (viewport, Help{"x, y, width, height as fractions of the target (of the split cell with split set), from its top-left corner"}),
              (target, Help{"a RenderTexture entity to draw into; empty: the window"}),
              (order, Help{"lower draws first (higher draws on top) within the target"}), enabled,
              (split, Range{0, 4}, Help{"1-4: draw into that player's split-screen cell; 0: no split"}))

// How the window splits between the players (M-VIEWS 4); one per scene (the first wins). Without
// one the layout is Auto with no divider.
enum class SplitLayout : u32 {
    Auto = 0,     // 2: side by side (stacked on a portrait window); 3: player 1 on top, 2 and 3 below; 4: a 2 x 2 grid
    Columns = 1,  // side by side
    Rows = 2,     // stacked
    Grid = 3,     // 2 x 2 (2 players side by side; with 3 the fourth cell stays empty)
};
struct SplitScreen {
    SplitLayout layout = SplitLayout::Auto;
    u32 divider = 0;  // pixels between the cells (the window's clear color shows through)
};
MCE_COMPONENT(SplitScreen, (layout, Help{"0 auto, 1 columns, 2 rows, 3 2x2 grid"}),
              (divider, Range{0, 64}, Help{"pixels between the cells"}))

// A UiScreen on the same entity lays out in `camera`'s view instead of the whole window (M-VIEWS
// 4): each player's HUD in their split cell. While that camera draws no window view the screen
// covers the window as before. Pointer input reaches it inside the view only.
struct UiScreenView {
    Entity camera;
};
MCE_COMPONENT(UiScreenView, (camera, Help{"the camera whose view the UI screen covers"}))

// Cell `index` (0-based) of a `count`-way split (1..4) as a fraction of the window (x, y, w, h,
// top-left origin). `aspect` = window width / height (Auto picks rows on a portrait window).
math::vec4 split_cell(SplitLayout layout, u32 count, u32 index, f32 aspect = 16.0f / 9.0f);

struct ViewEntry {
    Entity camera;
    Entity target;      // null = the window
    rhi::Rect2D rect;   // pixels of the target, top-left origin; never empty
    i32 order = 0;
    bool is_3d = false; // Camera3DComponent (else Camera2DComponent)
    u32 split = 0;      // CameraView::split (0 = none)
    rhi::Rect2D cell;   // the split cell in window pixels (the window when split = 0)
};

// A normalized rect as pixels of `size` (edges rounded, clamped to the target; may be empty).
rhi::Rect2D view_rect(const math::vec4& viewport, rhi::Extent2D size);

// The cost cuts of a view covering `area` of the window (0..1; see the rules above), applied to
// its built settings before Scene3D::build. `area` above 0.5 changes nothing.
void scale_view_quality(render::Scene3D& scene, f32 area);

// The frame's views, sorted as above. `window` is the window's size in pixels; a view into a
// texture gets its rect from `target_size(target entity)` (null: such views are left out, as are
// views whose rect comes out empty). False, with `out` empty, when no enabled CameraView exists.
using TargetSizeFn = rhi::Extent2D (*)(World& world, Entity target, void* user);
bool collect_views(World& world, rhi::Extent2D window, Array<ViewEntry>& out, TargetSizeFn target_size = nullptr,
                   void* user = nullptr);

class Kit3DExtract;
class SpriteExtract;
class RenderTextures;

// What record_views() draws with. `extract3d` null: 3D views are skipped (the host's 3D path is
// not running).
struct ViewRecordContext {
    render::Renderer* renderer = nullptr;
    Kit3DExtract* extract3d = nullptr;
    SpriteExtract* extract2d = nullptr;
    RenderTextures* render_textures = nullptr;  // views into textures (M-VIEWS 3); null: those are skipped
    rhi::TextureHandle target;            // the window's: null = backbuffer, or the editor's play view
    rhi::Extent2D target_size;
    math::Color clear{0.08f, 0.09f, 0.12f, 1.0f};
    f32 alpha = 1.0f;
    f32 frame_dt = 1.0f / 60.0f;          // the frame's first 3D extract takes it (Kit3DExtract::set_frame_dt)
    render::LodSettings lod;
    bool scale_quality = true;            // scale_view_quality by each view's share of the window
};
struct ViewRecordStats {
    u32 views_3d = 0;
    u32 views_2d = 0;
    u32 views_texture = 0;  // of those, drawn into render textures
    u32 window_3d = 0, window_2d = 0;      // of those, drawn into the window
    render::Scene3D::Stats first_3d;       // the window's first 3D view's scene
    render::BatchStats first_2d;           // the window's first 2D view's batch
};

// Records `views` (collect_views) in order: views into render textures first (each texture due a
// redraw cleared before its first view, drawn in linear color; see render_textures.h), then the
// window's, the window cleared once to ctx.clear first. A 3D view is its own Scene3D (extract, then extract_view for the later
// ones) in its own Forward3D view slot (Renderer::view_slot, keyed by the camera entity); a 2D view
// is a sprite batch drawn with the pass's viewport on its rect, cleared to its camera's clear color
// unless a 3D view drawn before it covers its rect (a HUD camera over a 3D world). Outside any pass.
// The caller ends the frame's views with Renderer::end_views().
ViewRecordStats record_views(rhi::CommandBuffer& cmd, World& world, Span<const ViewEntry> views, const ViewRecordContext& ctx);

}  // namespace mce::kit
