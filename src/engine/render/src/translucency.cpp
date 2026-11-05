#include "mce/render/translucency.h"

#include <cmath>

#include "mce/asset/formats.h"
#include "mce/render/quality.h"
#include "unit_clamp.h"

namespace mce::render {

f32 wrap_diffuse(f32 n_dot_l, f32 wrap) {
    if (n_dot_l != n_dot_l) return 0.0f;
    const f32 w = detail::ranged(wrap, 0.0f, 1.0f, 0.0f);
    const f32 x = n_dot_l + w;
    return (x > 0.0f ? x : 0.0f) / ((1.0f + w) * (1.0f + w));
}

f32 wrap_extra(f32 n_dot_l, f32 wrap) {
    if (n_dot_l != n_dot_l) return 0.0f;
    return wrap_diffuse(n_dot_l, wrap) - (n_dot_l > 0.0f ? n_dot_l : 0.0f);
}

f32 translucency_back(f32 n_dot_l, f32 v_dot_t, bool scatter) {
    const f32 nl = n_dot_l == n_dot_l ? n_dot_l : 0.0f;
    f32 r = -nl > 0.0f ? -nl : 0.0f;
    if (scatter) {
        const f32 vt = v_dot_t > 0.0f ? v_dot_t : 0.0f;  // NaN fails the compare: 0
        r += kTranslucencyScatter * std::pow(vt, kTranslucencyPower);
    }
    return r;
}

f32 translucency_object(u32 flags, bool unlit, const QualitySettings& q, bool renderer_on) {
    if (unlit || !renderer_on || !q.translucency || !translucency_cvar_on()) return 0.0f;
    const u32 t8 = (flags & asset::MaterialFlags::TranslucencyMask) >> asset::MaterialFlags::TranslucencyShift;
    const u32 w8 = (flags & asset::MaterialFlags::WrapMask) >> asset::MaterialFlags::WrapShift;
    return static_cast<f32>(t8 + 256u * w8);
}

TranslucencyFrame translucency_frame(const QualitySettings& q) {
    TranslucencyFrame f;
    if (!q.translucency || !translucency_cvar_on()) return f;
    f.ambient = q.translucency_ambient;
    f.scatter = q.translucency_scatter;
    return f;
}

}  // namespace mce::render
