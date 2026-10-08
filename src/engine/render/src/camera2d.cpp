#include "mce/render/camera2d.h"

#include <cmath>

namespace mce::render {

using namespace mce::math;

namespace {

f32 quantize_zoom(f32 zoom) {
    if (!(zoom > 0.0f)) return 1.0f;
    if (zoom >= 1.0f) return max(1.0f, std::round(zoom));
    return 1.0f / max(1.0f, std::round(1.0f / zoom));
}

f32 snap(f32 v, f32 step) { return std::round(v / step) * step; }

mat4 make_view_proj(vec2 center, f32 rotation, f32 half_w, f32 half_h) {
    const mat4 proj = ortho_rh_no(-half_w, half_w, -half_h, half_h, -1.0f, 1.0f);
    const mat4 view = rotation == 0.0f ? translation(vec3(-center.x, -center.y, 0.0f))
                                       : math::rotation(quat_from_axis_angle(vec3(0, 0, 1), -rotation)) *
                                             translation(vec3(-center.x, -center.y, 0.0f));
    return proj * view;
}

// Integer hash -> [-1, 1].
f32 hash_signed(u32 x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return static_cast<f32>(x & 0xFFFFFFu) / static_cast<f32>(0xFFFFFF) * 2.0f - 1.0f;
}

// 1D value noise, smoothstep-interpolated between integer lattice points.
f32 value_noise(u32 seed, u32 channel, f32 t) {
    const f32 fl = std::floor(t);
    const f32 f = t - fl;
    const auto i = static_cast<u32>(static_cast<i64>(fl));
    const u32 base = seed * 0x9E3779B9u + channel * 0x85EBCA6Bu;
    const f32 a = hash_signed(base + i);
    const f32 b = hash_signed(base + i + 1);
    const f32 s = f * f * (3.0f - 2.0f * f);
    return a + (b - a) * s;
}

}  // namespace

View2D compute_view_2d(const Camera2D& cam, rhi::Extent2D viewport, f32 pixels_per_meter, ShakeOffset shake) {
    View2D v;
    v.viewport = viewport;
    const f32 vw = static_cast<f32>(max(1u, viewport.width));
    const f32 vh = static_cast<f32>(max(1u, viewport.height));
    const f32 ppm = pixels_per_meter > 0.0f ? pixels_per_meter : 48.0f;
    v.effective_zoom = cam.pixel_snap ? quantize_zoom(cam.zoom) : (cam.zoom > 0.0f ? cam.zoom : 1.0f);
    v.pixels_per_meter = ppm * v.effective_zoom;

    vec2 center = cam.position;
    if (cam.pixel_snap) {
        const f32 px = 1.0f / v.pixels_per_meter;
        center = vec2(snap(center.x, px), snap(center.y, px));
        // An odd viewport puts NDC 0 in the middle of a pixel: move half a pixel so texel edges
        // still land on pixel edges.
        if (max(1u, viewport.width) % 2 == 1) center.x += 0.5f * px;
        if (max(1u, viewport.height) % 2 == 1) center.y += 0.5f * px;
    }
    center = center + shake.translation;
    const f32 rotation = cam.rotation + shake.rotation;
    v.center = center;

    const f32 half_w = 0.5f * vw / v.pixels_per_meter;
    const f32 half_h = 0.5f * vh / v.pixels_per_meter;
    v.view_proj = make_view_proj(center, rotation, half_w, half_h);
    if (rotation == 0.0f) {
        v.world_bounds = {center - vec2(half_w, half_h), center + vec2(half_w, half_h)};
    } else {
        const f32 r = std::sqrt(half_w * half_w + half_h * half_h);
        v.world_bounds = {center - vec2(r), center + vec2(r)};
    }
    return v;
}

View2D compute_screen_view(rhi::Extent2D viewport) {
    View2D v;
    v.viewport = viewport;
    const f32 w = static_cast<f32>(max(1u, viewport.width));
    const f32 h = static_cast<f32>(max(1u, viewport.height));
    v.view_proj = ortho_rh_no(0.0f, w, h, 0.0f, -1.0f, 1.0f);
    v.world_bounds = {vec2(0, 0), vec2(w, h)};
    v.center = vec2(0.5f * w, 0.5f * h);
    v.pixels_per_meter = 1.0f;
    return v;
}

void ScreenShake::add_trauma(f32 amount) { trauma_ = clamp(trauma_ + amount, 0.0f, 1.0f); }

void ScreenShake::tick(f32 dt) {
    time_ += dt;
    trauma_ = max(0.0f, trauma_ - decay_per_second * dt);
}

ShakeOffset ScreenShake::offset() const {
    ShakeOffset o;
    if (trauma_ <= 0.0f) return o;
    const f32 k = trauma_ * trauma_;
    const f32 t = time_ * frequency_hz;
    o.translation = vec2(value_noise(seed_, 0, t), value_noise(seed_, 1, t)) * (k * max_translation);
    o.rotation = value_noise(seed_, 2, t) * k * max_rotation;
    return o;
}

}  // namespace mce::render
