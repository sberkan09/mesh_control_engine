#pragma once
// The Minimap widget's maths (M-MISSION step 2), pure and headless: where a world position lands in a
// widget's rect. Document::submit draws with these; the tests and any host call them directly.
//
// World positions are (x, z) metres on the ground plane, x east, z south. Heading is yaw about +Y in
// radians: 0 faces north (-z) and a positive heading turns toward the west, so the forward direction is
// (-sin h, -cos h). North-up: east is right, north is up. Heading-up (rotate): forward is up.
//
// The visible region is the circle of `radius` metres around the tracked position, inscribed in the rect
// (its radius in pixels is half the shorter side); pixels per metre = that / radius. A position farther than
// `radius` is `inside = false`; with clamp_to_rim it lands on the circle instead (`on_rim = true`, along the
// line from the centre).

#include "mce/containers/array.h"
#include "mce/ui/ui.h"

namespace mce::ui {

struct MinimapPoint {
    vec2 point;            // widget space: pixels, +y down
    bool inside = true;    // within the radius (full map: within the world rectangle)
    bool on_rim = false;   // clamped to the rim
};

MinimapPoint minimap_project(vec2 world, vec2 tracked, f32 heading, bool rotate_with_heading, f32 radius, const Rect& rect,
                             bool clamp_to_rim);

// Full-map mode: world_min .. world_max fills the rect (north up). Outside the rectangle is `inside = false`
// with the unclamped point.
MinimapPoint minimap_project_full(vec2 world, vec2 world_min, vec2 world_max, const Rect& rect);

// The radius a widget shows: radius / zoom.
inline f32 minimap_radius(const Minimap& m) { return m.radius / (m.zoom > 0.0f ? m.zoom : 1.0f); }

struct MinimapShown {
    u32 marker = 0;        // index into the markers given
    vec2 point;
    bool on_rim = false;
};

// Which markers a widget draws and where, in marker order: the layer mask, the radius (or world rectangle in
// full-map mode) and the rim rules applied. A marker beyond the radius shows on the rim when the widget's
// clamp_to_rim is on or the marker is always_visible, and is dropped otherwise.
void minimap_layout(const Minimap& m, const MinimapView& view, Span<const MinimapMarker> markers, const Rect& rect,
                    Array<MinimapShown>& out);

}  // namespace mce::ui
