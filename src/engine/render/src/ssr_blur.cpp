// M-REFLECT2 48b: the SSR roughness blur radius; kMeshFS (shaders3d.h, ssr_blur_radius / ssr_bump_term) is its twin.
#include "mce/render/ssr_blur.h"

namespace mce::render {

f32 ssr_blur_radius(f32 rough, f32 bump) {
    if (!(rough > kSsrBlurMinRough)) return 0.0f;  // NaN fails the comparison too
    const f32 r = rough < 1.0f ? rough : 1.0f;
    const f32 b = bump > 0.0f ? bump : 0.0f;
    const f32 a = r * r - kSsrBlurMinRough * kSsrBlurMinRough + b;
    const f32 radius = kSsrBlurScale * a;
    return radius < kSsrBlurMax ? radius : kSsrBlurMax;
}

f32 ssr_bump_term(f32 n_dot_geo) {
    if (!(n_dot_geo == n_dot_geo)) return 0.0f;
    const f32 d = n_dot_geo > 1.0f ? 1.0f : n_dot_geo < -1.0f ? -1.0f : n_dot_geo;
    return kSsrBumpGain * (1.0f - d);
}

}  // namespace mce::render
