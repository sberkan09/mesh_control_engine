#pragma once
// 2D camera interpretation (rhi-renderer.md §2.5, rendering-2d §9, FR-R2D-040).
//
// The Camera2D component itself belongs to the gameplay starter kit (FR-ENT-080); this is the
// renderer's reading of it. Conventions: meters, Y up, right-handed, GL clip z in [-1, 1]; depth
// is unused in 2D (painter's order through the sort key).

#include "mce/math/math.h"
#include "mce/rhi/types.h"

namespace mce::render {

struct Camera2D {
    math::vec2 position;     // world center of view
    f32 zoom = 1.0f;         // 1 = one art texel per device pixel at the set's pixels-per-meter
    f32 rotation = 0.0f;     // radians, CCW
    bool pixel_snap = true;  // FR-R2D-040: snap the view to the device-pixel grid
    // Fit mode: when either is > 0 the view shows at least this many world metres across / up
    // (both set: the whole fit_width x fit_height rect, letterboxed by the longer axis), whatever
    // the viewport's pixel count; zoom and its pixel-snap quantization are then ignored.
    f32 fit_width = 0.0f;
    f32 fit_height = 0.0f;
};

// The resolved view: everything extract/cull/record need.
struct View2D {
    math::mat4 view_proj;
    math::aabb2 world_bounds;     // conservative visible rectangle (covers rotation)
    math::vec2 center;            // after snapping + shake
    f32 pixels_per_meter = 48.0f; // device pixels per world meter (ppm × effective zoom)
    f32 effective_zoom = 1.0f;    // zoom after pixel-snap quantization
    rhi::Extent2D viewport;
};

// Post-snap offsets from the screen-shake helper (applied after snapping: shake may shimmer,
// game-critical art may not).
struct ShakeOffset {
    math::vec2 translation;  // meters
    f32 rotation = 0.0f;     // radians
};

// `pixels_per_meter` is the art density (sprite-set PPM, default 48; rendering-2d §9).
// With pixel_snap: zoom is quantized to an integer (>= 1) or 1/integer (< 1) device-pixels-per-
// texel ratio and the view center is snapped so texel edges land on device-pixel edges.
View2D compute_view_2d(const Camera2D& cam, rhi::Extent2D viewport, f32 pixels_per_meter = 48.0f,
                       ShakeOffset shake = {});

// A pixel-space view for HUD/debug text: origin top-left, +y down, one unit per device pixel.
View2D compute_screen_view(rhi::Extent2D viewport);

// Trauma-style screen shake (FR-R2D-040 "screen-shake helper"). Deterministic: the noise is a
// hash of (seed, tick), never wall-clock, so replays shake identically. Advance it on the fixed
// tick; read offset() when building the view.
class ScreenShake {
public:
    explicit ScreenShake(u32 seed = 0x5EED) : seed_(seed) {}

    void add_trauma(f32 amount);  // trauma is clamped to [0, 1]
    void tick(f32 dt);            // decays trauma linearly by `decay_per_second`
    ShakeOffset offset() const;   // magnitude = trauma² × max_*
    f32 trauma() const { return trauma_; }

    f32 max_translation = 0.5f;   // meters
    f32 max_rotation = 0.05f;     // radians
    f32 decay_per_second = 1.5f;
    f32 frequency_hz = 15.0f;     // how fast the offset changes

private:
    u32 seed_;
    f32 time_ = 0.0f;
    f32 trauma_ = 0.0f;
};

}  // namespace mce::render
