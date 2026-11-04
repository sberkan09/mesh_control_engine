#include "mce/render/light_cookie.h"

#include <cmath>

#include "mce/render/clusters.h"
#include "mce/render/scene3d.h"
#include "unit_clamp.h"

namespace mce::render {

namespace {

constexpr f32 kNearZ = 0.05f;
constexpr f32 kVerticalCos = 0.99985f;  // cos(1 degree): a cone or sun this close to vertical takes +Z as its up hint

f32 smooth_step(f32 a, f32 b, f32 x) {
    const f32 t = detail::ranged((x - a) / (b - a), 0.0f, 1.0f, 0.0f);
    return t * t * (3.0f - 2.0f * t);
}

bool finite3(math::vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// A unit direction; a non-finite or (near) zero one is straight down.
math::vec3 unit_or_down(math::vec3 d) {
    const f32 len2 = d.x * d.x + d.y * d.y + d.z * d.z;
    if (!finite3(d) || !(len2 >= 1.0e-12f) || len2 > 3.0e38f) return math::vec3{0.0f, -1.0f, 0.0f};
    const f32 len = std::sqrt(len2);
    return math::vec3{d.x / len, d.y / len, d.z / len};
}

math::vec3 up_hint(math::vec3 unit_dir) {
    return std::fabs(unit_dir.y) > kVerticalCos ? math::vec3{0.0f, 0.0f, 1.0f} : math::vec3{0.0f, 1.0f, 0.0f};
}

f32 finite_or_zero(f32 x) { return std::isfinite(x) ? x : 0.0f; }

f32 cone_half_angle(f32 spot_outer) { return detail::ranged(spot_outer, 0.01f, 1.45f, 0.5f); }

// The mip for a sample whose texel covers `texel_m` world metres at a footprint of `footprint_m`, clamped to a chain of
// `texels` texels per side.
f32 lod_of(f32 footprint_m, f32 texel_m, u32 texels) {
    if (texels == 0u || !std::isfinite(footprint_m) || !std::isfinite(texel_m) || !(footprint_m > 0.0f) || !(texel_m > 0.0f))
        return 0.0f;
    const f32 ratio = footprint_m / texel_m;
    const f32 lod = std::log2(ratio > 1.0f ? ratio : 1.0f);
    const f32 top = std::log2(static_cast<f32>(texels));
    return std::isfinite(lod) ? detail::ranged(lod, 0.0f, top, 0.0f) : 0.0f;
}

}  // namespace

math::mat4 spot_cookie_matrix(math::vec3 position, math::vec3 direction, f32 spot_outer, f32 roll, f32 radius) {
    const math::vec3 dir = unit_or_down(direction);
    const math::vec3 right = math::normalize(math::cross(dir, up_hint(dir)));
    const math::vec3 up = math::cross(right, dir);
    // A roll turns the pattern counter-clockwise seen from the light: the view's up leans toward -right.
    const f32 r = finite_or_zero(roll);
    const f32 c = std::cos(r), s = std::sin(r);
    const math::vec3 rolled_up = up * c - right * s;
    const f32 far_z = radius == radius ? (radius > 0.1f ? radius : 0.1f) : 5.0f;
    const math::mat4 view = math::look_at_rh(position, position + dir, rolled_up);
    const math::mat4 proj = math::perspective_rh_no(2.0f * cone_half_angle(spot_outer), 1.0f, kNearZ, far_z);
    return proj * view;
}

f32 spot_cookie_edge(math::vec2 uv, f32 edge) {
    const f32 e = detail::ranged(edge, 0.0f, 1.0f, 0.0f);
    if (!(e > 0.0f)) return 1.0f;
    if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) return 0.0f;
    const f32 fx = uv.x < 1.0f - uv.x ? uv.x : 1.0f - uv.x;
    const f32 fy = uv.y < 1.0f - uv.y ? uv.y : 1.0f - uv.y;
    return smooth_step(0.0f, e * 0.5f, fx < fy ? fx : fy);
}

math::vec2 sun_cookie_uv(math::vec3 world, math::vec3 sun_dir, const SunCookie& cookie) {
    const math::vec3 d = unit_or_down(sun_dir);
    const math::vec3 right = math::normalize(math::cross(up_hint(d), d));
    const math::vec3 up = math::cross(d, right);
    const f32 px = math::dot(world, right);
    const f32 py = math::dot(world, up);
    const f32 r = finite_or_zero(cookie.roll);
    const f32 c = std::cos(r), s = std::sin(r);
    const f32 size = detail::ranged(cookie.size, 0.5f, 1000.0f, 20.0f);
    const f32 ox = finite_or_zero(cookie.offset.x), oy = finite_or_zero(cookie.offset.y);
    return math::vec2{(c * px + s * py) / size + ox / size, (-s * px + c * py) / size + oy / size};
}

f32 cookie_lod(f32 pixel_footprint_m, f32 distance_m, f32 spot_outer, u32 size) {
    if (!std::isfinite(distance_m) || !(distance_m > 0.0f) || size == 0u) return 0.0f;
    const f32 texel_m = 2.0f * distance_m * std::tan(cone_half_angle(spot_outer)) / static_cast<f32>(size);
    return lod_of(pixel_footprint_m, texel_m, size);
}

f32 cookie_lod_sun(f32 pixel_footprint_m, f32 size_m, u32 texels) {
    if (texels == 0u) return 0.0f;
    return lod_of(pixel_footprint_m, size_m / static_cast<f32>(texels), texels);
}

CookiePlan plan_cookies(Span<const PointLight> lights, const DirectionalLight& sun, u32 max_layers, u32 max_lights,
                        Span<u32> light_record) {
    CookiePlan plan;
    const u32 layer_cap = max_layers < kMaxCookieLayers ? max_layers : kMaxCookieLayers;
    const u32 light_cap = max_lights < kMaxCookieLights ? max_lights : kMaxCookieLights;
    for (u32& r : light_record) r = ~0u;

    auto layer_for = [&](u32 key) -> u32 {
        for (u32 i = 0; i < plan.count; ++i)
            if (plan.key[i] == key) return i;
        if (plan.count >= layer_cap) return ~0u;
        plan.key[plan.count] = key;
        return plan.count++;
    };

    if (sun.cookie.texture != ~0u && sun.cookie.strength > 0.0f) plan.sun_layer = layer_for(sun.cookie.texture);

    for (usize i = 0; i < lights.size(); ++i) {
        const PointLight& l = lights[i];
        if (!(l.spot_outer > 0.0f) || l.cookie == ~0u || !(l.cookie_strength > 0.0f)) continue;
        if (plan.lights >= light_cap) {
            ++plan.dropped;
            continue;
        }
        const u32 layer = layer_for(l.cookie);
        if (layer == ~0u) {
            ++plan.dropped;
            continue;
        }
        plan.layer_of[plan.lights] = layer;
        if (i < light_record.size()) light_record[i] = plan.lights;
        ++plan.lights;
    }
    return plan;
}

GpuCookieRecord cookie_record(const PointLight& light, u32 layer) {
    GpuCookieRecord rec;
    rec.view_proj = spot_cookie_matrix(light.position, light.direction, light.spot_outer, light.cookie_roll, light.radius);
    rec.params = math::vec4{static_cast<f32>(layer), detail::ranged(light.cookie_strength, 0.0f, 1.0f, 1.0f),
                            detail::ranged(light.cookie_edge, 0.0f, 1.0f, 0.0f), 0.0f};
    return rec;
}

}  // namespace mce::render
