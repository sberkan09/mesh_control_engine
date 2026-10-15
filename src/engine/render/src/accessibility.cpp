#include "mce/render/accessibility.h"

#include "mce/convar.h"
#include "mce/render/renderer.h"
#include "mce/render/scene3d.h"

namespace mce::render {
namespace {

MCE_CVAR(cv_colorblind, "access.colorblind", 0,
         "Color-vision correction: 0 none, 1 protanopia, 2 deuteranopia, 3 tritanopia (never in the state hash)",
         CVarFlags::Saved);
MCE_CVAR(cv_colorblind_strength, "access.colorblind_strength", 1.0f, "How much of the color-vision correction to apply (0..1)",
         CVarFlags::Saved);
MCE_CVAR(cv_text_scale, "access.text_scale", 1.0f, "UI text size on top of ui_scale (0.5..3)", CVarFlags::Saved);
MCE_CVAR(cv_high_contrast, "access.high_contrast", false, "Draw every UI document with the high-contrast palette",
         CVarFlags::Saved);
MCE_CVAR(cv_motion_scale, "access.motion_scale", 1.0f,
         "Camera shake, flashes and screen effects (0 = none, 1 = as the game made them; never in the state hash)",
         CVarFlags::Saved);

// Machado, Oliveira & Fernandes (2009), severity 1, linear RGB; rows. Keep in step with the
// shaders' copies (renderer.cpp kPostFS, shaders3d.h MCE_GLSL_CVD).
constexpr f32 kSimulate[kColorVisionCount - 1][3][3] = {
    {{0.152286f, 1.052583f, -0.204868f}, {0.114503f, 0.786281f, 0.099216f}, {-0.003882f, -0.048116f, 1.051998f}},
    {{0.367322f, 0.860646f, -0.227968f}, {0.280085f, 0.672501f, 0.047413f}, {-0.011820f, 0.042940f, 0.968881f}},
    {{1.255528f, -0.076749f, -0.178779f}, {-0.078411f, 0.930809f, 0.147602f}, {0.004733f, 0.691367f, 0.303900f}},
};

f32 clamp01(f32 v) { return math::clamp(v, 0.0f, 1.0f); }

}  // namespace

const char* color_vision_name(ColorVision v) {
    switch (v) {
        case ColorVision::None: return "none";
        case ColorVision::Protanopia: return "protanopia";
        case ColorVision::Deuteranopia: return "deuteranopia";
        case ColorVision::Tritanopia: return "tritanopia";
    }
    return "none";
}

AccessibilitySettings accessibility_settings() {
    AccessibilitySettings s;
    const i32 cv = cv_colorblind.get_int();
    s.color_vision = cv >= 1 && cv < static_cast<i32>(kColorVisionCount) ? static_cast<ColorVision>(cv) : ColorVision::None;
    s.color_vision_strength = clamp01(cv_colorblind_strength.get_float());
    s.text_scale = math::clamp(cv_text_scale.get_float(), 0.5f, 3.0f);
    s.high_contrast = cv_high_contrast.get_bool();
    s.motion_scale = clamp01(cv_motion_scale.get_float());
    return s;
}

math::vec3 simulate_color_vision(ColorVision v, math::vec3 c) {
    if (v == ColorVision::None) return c;
    const f32(*m)[3] = kSimulate[static_cast<u32>(v) - 1];
    return {m[0][0] * c.x + m[0][1] * c.y + m[0][2] * c.z, m[1][0] * c.x + m[1][1] * c.y + m[1][2] * c.z,
            m[2][0] * c.x + m[2][1] * c.y + m[2][2] * c.z};
}

math::vec3 correct_color_vision(ColorVision v, f32 strength, math::vec3 c) {
    if (v == ColorVision::None || strength <= 0.0f) return c;
    const math::vec3 s = simulate_color_vision(v, c);
    const math::vec3 e{c.x - s.x, c.y - s.y, c.z - s.z};
    // The missed color moves into the channels the viewer still tells apart.
    math::vec3 shift;
    if (v == ColorVision::Tritanopia)
        shift = {e.x + 0.7f * e.z, e.y + 0.7f * e.z, 0.0f};
    else
        shift = {0.0f, 0.7f * e.x + e.y, 0.7f * e.x + e.z};
    const f32 k = clamp01(strength);
    return {clamp01(c.x + shift.x * k), clamp01(c.y + shift.y * k), clamp01(c.z + shift.z * k)};
}

void apply_accessibility(const AccessibilitySettings& s, Post2D& post) {
    post.color_vision = s.color_vision_strength > 0.0f ? s.color_vision : ColorVision::None;
    post.color_vision_strength = s.color_vision_strength;
    post.aberration *= s.motion_scale;
    if (s.motion_scale <= 0.0f) post.chromatic_aberration = false;
}

void apply_accessibility(const AccessibilitySettings& s, PostFx& post) {
    post.color_vision = s.color_vision_strength > 0.0f ? s.color_vision : ColorVision::None;
    post.color_vision_strength = s.color_vision_strength;
    post.motion_blur.intensity *= s.motion_scale;
    if (s.motion_scale <= 0.0f) post.motion_blur.enabled = false;
}

}  // namespace mce::render
