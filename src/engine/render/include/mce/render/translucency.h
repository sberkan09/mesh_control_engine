#pragma once
// M-TRANSLUCENT 41b: the pure CPU helpers of thin-surface translucency and wrap lighting. wrap_diffuse, wrap_extra and
// translucency_back are the reference the 41d and 41e fragment shaders mirror literally, one expression each (with the
// three constants below); translucency_object is the value `ObjectU::o_skin.w` receives and translucency_frame the
// per-frame switches of `PassU::p_translucency`. f32 only, fixed evaluation order, no rand, no state. Included by
// translucency.cpp, forward3d.cpp, quality.cpp and the tests only.
//
// Narrow on purpose: it includes mce/math only, so QualitySettings is forward-declared.
//
// Conventions. n_dot_l is dot(n, l) with l pointing from the surface toward the light, so it is negative when the light
// is on the back of the surface. The wrap is Filament's energy-conserving form. The back term is the Lambert lobe around
// the inverted normal, -n.l, plus (when `scatter`) a forward-scatter lobe toward the light: with
// v_dot_t = dot(v, -normalize(l + n * kTranslucencyDistortion)), kTranslucencyScatter * pow(max(v_dot_t, 0), kTranslucencyPower).

#include "mce/math/math.h"
#include "mce/types.h"

namespace mce::render {

struct QualitySettings;

// How far the forward-scatter direction bends toward the normal, the lobe's tightness and its weight against the Lambert
// lobe. Constants, not material fields (no cooked bits are left); look choices to be settled on a real display.
inline constexpr f32 kTranslucencyDistortion = 0.2f;
inline constexpr f32 kTranslucencyPower = 4.0f;
inline constexpr f32 kTranslucencyScatter = 0.5f;

// max(n_dot_l + wrap, 0) / ((1 + wrap) * (1 + wrap)). wrap is clamped to 0..1 (NaN 0); wrap_diffuse(x, 0) == max(x, 0)
// exactly. A NaN n_dot_l gives 0.
f32 wrap_diffuse(f32 n_dot_l, f32 wrap);

// wrap_diffuse(n_dot_l, wrap) - max(n_dot_l, 0): what the lit pass adds to the plain clamped Lambert (negative on the lit
// side, positive in the band past the terminator); exactly 0 for wrap 0.
f32 wrap_extra(f32 n_dot_l, f32 wrap);

// max(-n_dot_l, 0) + (scatter ? kTranslucencyScatter * pow(max(v_dot_t, 0), kTranslucencyPower) : 0). 0 for a front-lit
// point without the lobe. A NaN input counts as 0.
f32 translucency_back(f32 n_dot_l, f32 v_dot_t, bool scatter);

// The value ObjectU::o_skin.w receives for a draw whose material flags word is `flags`: t8 + 256 * w8, with t8 and w8 the
// 8-bit translucency and wrap fields (MaterialFlags::TranslucencyMask / WrapMask), an exact integer 0..65535. Exactly 0
// for an unlit material, both fields 0, !q.translucency, `r.translucency 0` or !renderer_on (Forward3DDesc::translucency).
f32 translucency_object(u32 flags, bool unlit, const QualitySettings& q, bool renderer_on);

// The value of `r.translucency` (registered in quality.cpp, next to r.light_cookies).
bool translucency_cvar_on();

// The per-frame switches (PassU::p_translucency.xy): the back side's ambient and the forward-scatter lobe. Both false when
// the tier or `r.translucency 0` says off.
struct TranslucencyFrame {
    bool ambient = false;
    bool scatter = false;
};
TranslucencyFrame translucency_frame(const QualitySettings& q);

}  // namespace mce::render
