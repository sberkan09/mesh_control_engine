#pragma once
// Player accessibility settings (M-ACCESS step 2): color-vision correction, UI text scale, a
// high-contrast UI and reduced motion. They are presentation only: console variables (Saved, so a
// settings menu can bind them), read by the extractors and the player each frame and never written
// into components, so two players with different settings keep the same state hash.
//
//   access.colorblind           0 none, 1 protanopia, 2 deuteranopia, 3 tritanopia
//   access.colorblind_strength  0..1, how much of the correction to apply
//   access.text_scale           UI text size on top of ui_scale (0.5..3)
//   access.high_contrast        the high-contrast UI palette over every document
//   access.motion_scale         0..1, camera shake, flashes and screen effects (0 = none)

#include "mce/math/math.h"
#include "mce/render/camera2d.h"
#include "mce/types.h"

namespace mce::render {

struct Post2D;
struct PostFx;

// The color-vision deficiency the frame is corrected for.
enum class ColorVision : u8 { None = 0, Protanopia, Deuteranopia, Tritanopia };
inline constexpr u32 kColorVisionCount = 4;

const char* color_vision_name(ColorVision v);  // "none", "protanopia", "deuteranopia", "tritanopia"

struct AccessibilitySettings {
    ColorVision color_vision = ColorVision::None;
    f32 color_vision_strength = 1.0f;  // 0..1
    f32 text_scale = 1.0f;             // 0.5..3
    bool high_contrast = false;
    f32 motion_scale = 1.0f;           // 0..1
};

// The current settings from the access.* convars, clamped to their ranges.
AccessibilitySettings accessibility_settings();

// Daltonization (Fidaner, Lin & Ozguven) over the Machado, Oliveira & Fernandes (2009) severity-1
// simulation: the color a viewer with `v` misses (c - simulate(c)) is shifted into the channels they
// still see, `strength` of it. Linear RGB in and out, clamped to [0, 1]. The 2D post pass and the 3D
// tonemap pass run the same math on the GPU (renderer.cpp kPostFS, shaders3d.h kTonemapFS); this
// is the reference the tests hold them to.
math::vec3 simulate_color_vision(ColorVision v, math::vec3 linear_rgb);
math::vec3 correct_color_vision(ColorVision v, f32 strength, math::vec3 linear_rgb);

// Apply the settings to a frame's post chain. 2D: the color-vision correction runs in the post pass
// (so post.any() turns true with it on) and the chromatic aberration is scaled by motion_scale.
// 3D: the correction runs in the tonemap pass; motion blur is scaled by motion_scale.
void apply_accessibility(const AccessibilitySettings& s, Post2D& post);
void apply_accessibility(const AccessibilitySettings& s, PostFx& post);

// A camera shake offset scaled for reduced motion (motion_scale 0 = still).
inline ShakeOffset scale_shake(ShakeOffset shake, f32 motion_scale) {
    return ShakeOffset{shake.translation * motion_scale, shake.rotation * motion_scale};
}

}  // namespace mce::render
