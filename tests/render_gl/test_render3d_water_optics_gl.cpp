// M-WATER2 49c / 49d against a real driver (GL 4.5 on llvmpipe and, as test_render_sdlgpu, the SDL GPU backend on
// lavapipe): the water's refraction of what lies under it and its screen-space reflection of what stands above it
// (kWaterFS, p_water6, the opaque copy at bank 7). 49c: a striped seabed shows through a calm surface where the seabed
// itself would be (no bend at amplitude 0), the waves bend it by at most kWaterRefractMaxUv, red dies first with depth,
// deep water ends at the old body colour, a post's dry part never leaks into the water around it, glass over the water
// still shows the water. 49d: an emissive pillar's mirror image lies at the analytic mirror position, widens with the
// waves, fades continuously out at the screen edge; a scene with nothing above the water and a pillar under it reflect
// the sky bit for bit. Off: the Low look, a renderer without the prepass pipelines and the camera under the level are
// byte-identical with the two cvars on and off and break no pass. Each shot is a 160 x 120 frame, bloom and FXAA off.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "../render/mesh_fixture.h"
#include "fixture3d.h"
#include "mce/render/water_optics.h"

using namespace mce;
using namespace mce::render;
using math::vec3;
using namespace mce::render_test;

namespace {

constexpr u32 kStripes = 40, kPost = 41, kPillar = 42, kGlass = 43;

template <class T>
bool same(const Array<T>& a, const Array<T>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

f64 luma(const Array<u8>& px, usize i) { return 0.2126 * px[i] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2]; }

// A saturated green the scenes have nowhere else (the pillar and the post's dry part).
bool greenish(const Array<u8>& px, usize i) { return px[i + 1] > px[i] + 50 && px[i + 1] > px[i + 2] + 50; }

// 24 strips of 0.5 m across z over the ground slab, white and near black in turn (a seabed to see through the water).
void upload_stripes(Fixture3D& f) {
    mesh_fixture::Builder b;
    b.add_node(-1);
    for (u32 i = 0; i < 24; ++i) {
        mesh_fixture::Box s;
        s.center = {0.0f, 0.01f, -6.0f + 0.25f + 0.5f * f32(i)};
        s.half = {6.0f, 0.01f, 0.25f};
        const f32 c = (i & 1) ? 0.9f : 0.04f;
        s.color[0] = s.color[1] = s.color[2] = c;
        b.add_box(s);
    }
    REQUIRE(f.renderer.meshes().upload(kStripes, 1, b.build().span()).has_value());
}

// A post at (-1.5, 1): grey from the ground to y 0.6, saturated green from 0.6 to 1.6.
void upload_post(Fixture3D& f) {
    mesh_fixture::Builder b;
    b.add_node(-1);
    mesh_fixture::Box lo;
    lo.center = {-1.5f, 0.3f, 1.0f};
    lo.half = {0.2f, 0.3f, 0.2f};
    lo.color[0] = lo.color[1] = lo.color[2] = 0.5f;
    b.add_box(lo);
    mesh_fixture::Box hi;
    hi.center = {-1.5f, 1.1f, 1.0f};
    hi.half = {0.2f, 0.5f, 0.2f};
    hi.color[0] = 0.03f;
    hi.color[1] = 0.9f;
    hi.color[2] = 0.03f;
    b.add_box(hi);
    REQUIRE(f.renderer.meshes().upload(kPost, 1, b.build().span()).has_value());
}

// A green emissive pillar 0.6 m square standing on the ground from y `bottom` to `top`, its front face at z -2.2.
constexpr f32 kPillarFront = -2.2f;
constexpr f32 kPillarX = 1.8f;  // clear of the fixture's box, which stands at the origin
void upload_pillar(Fixture3D& f, f32 x = kPillarX, f32 bottom = 0.0f, f32 top = 2.5f) {
    mesh_fixture::Builder b;
    b.add_node(-1);
    mesh_fixture::Box p;
    p.center = {x, 0.5f * (bottom + top), kPillarFront - 0.3f};
    p.half = {0.3f, 0.5f * (top - bottom), 0.3f};
    p.color[0] = p.color[1] = p.color[2] = 0.0f;
    b.add_box(p);
    b.submeshes.back().material.emissive[1] = 3.0f;
    REQUIRE(f.renderer.meshes().upload(kPillar, 1, b.build().span()).has_value());
}

void add_mesh(Scene3D& s, u32 mesh) {
    MeshInstance m;
    m.mesh = mesh;
    s.add(m);
}

// A dark body and no refraction: what the water shows is the reflection (the reflection cases).
WaterSurface dark(WaterSurface w) {
    w.shallow_color = w.deep_color = {0.0f, 0.0f, 0.0f};
    return w;
}

WaterSurface sea(f32 level, f32 amplitude, f32 foam = 0.05f) {
    WaterSurface w;
    w.enabled = true;
    w.level_y = level;
    w.amplitude = amplitude;
    w.wavelength = 4.0f;
    w.foam_amount = foam;  // > 0: the depth is read even on a calm sea
    w.foam_width_m = 0.05f;
    w.time = 12.5f;
    return w;
}

struct Shot {
    Array<u8> px;
    Forward3DStats st;
};

// One frame of `w` (null: no water) plus `more`; the fixture's ground, box and sun; bloom and FXAA off.
Shot shoot(Fixture3D& f, const WaterSurface* w, const std::function<void(Scene3D&)>& more = {}, bool reflect = true,
           bool refract = true) {
    const WaterOptics fx(reflect, refract);
    f.render(0.0f, false, [&](Scene3D& s) {
        PostFx post = s.post();
        post.bloom = false;
        post.fxaa = false;
        s.set_post(post);
        s.set_exposure(0.7f);
        if (w) s.set_water(*w);
        if (more) more(s);
    });
    return {f.pixels, f.renderer.forward_3d().last_stats()};
}

// The view ray through a pixel's centre (top row first).
vec3 ray_dir(const Fixture3D& f, u32 x, u32 y) {
    const math::mat4 inv = math::inverse(f.view.view_proj);
    const f32 nx = (f32(x) + 0.5f) / f32(kW) * 2.0f - 1.0f, ny = 1.0f - (f32(y) + 0.5f) / f32(kH) * 2.0f;
    const math::vec4 w = inv * math::vec4(nx, ny, 1.0f, 1.0f);
    return math::normalize(vec3{w.x / w.w, w.y / w.w, w.z / w.w} - f.view.position);
}

// Where a pixel's ray meets the plane y = level, or false.
bool on_plane(const Fixture3D& f, u32 x, u32 y, f32 level, vec3& at) {
    const vec3 d = ray_dir(f, x, y);
    if (d.y >= -1e-4f) return false;
    at = f.view.position + d * ((level - f.view.position.y) / d.y);
    return true;
}

// The screen row and column of a world point (fractional, top row first).
math::vec2 screen_of(const Fixture3D& f, vec3 p) {
    const math::vec4 c = f.view.view_proj * math::vec4(p, 1.0f);
    return {(c.x / c.w * 0.5f + 0.5f) * f32(kW), (0.5f - c.y / c.w * 0.5f) * f32(kH)};
}

// Rows of column x whose ray meets the water over the striped part of the slab (away from its edges and the box).
Array<u32> stripe_rows(const Fixture3D& f, u32 x, f32 level) {
    Array<u32> rows;
    for (u32 y = 0; y < kH; ++y) {
        vec3 at;
        if (!on_plane(f, x, y, level, at)) continue;
        if (std::fabs(at.x) < 5.0f && std::fabs(at.z) < 5.0f && !(std::fabs(at.x) < 1.0f && std::fabs(at.z) < 1.0f))
            rows.push_back(y);
    }
    return rows;
}

f64 column_stddev(const Array<u8>& px, u32 x, const Array<u32>& rows) {
    f64 s = 0.0, s2 = 0.0;
    for (u32 y : rows) {
        const f64 l = luma(px, (usize(y) * kW + x) * 4);
        s += l;
        s2 += l * l;
    }
    const f64 n = f64(rows.size());
    return std::sqrt(std::max(0.0, s2 / n - (s / n) * (s / n)));
}

// Whether each row is brighter than the column's median over `rows`.
Array<bool> bright_rows(const Array<u8>& px, u32 x, const Array<u32>& rows) {
    Array<f64> l;
    for (u32 y : rows) l.push_back(luma(px, (usize(y) * kW + x) * 4));
    Array<f64> sorted = l;
    std::sort(sorted.begin(), sorted.end());
    const f64 median = sorted[sorted.size() / 2];
    Array<bool> b;
    for (f64 v : l) b.push_back(v > median);
    return b;
}

}  // namespace

TEST_CASE("water optics: a striped seabed shows through calm water where it lies, and the waves bend it a little") {
    Fixture3D f;
    upload_stripes(f);
    const auto stripes = [](Scene3D& s) { add_mesh(s, kStripes); };
    const f32 level = 0.8f;
    const WaterSurface calm = sea(level, 0.0f);
    const Shot dry = shoot(f, nullptr, stripes);
    const Shot on = shoot(f, &calm, stripes);
    CHECK(on.st.water_refract);
    CHECK(on.st.water_reflect);
    const Shot off = shoot(f, &calm, stripes, true, false);
    CHECK_FALSE(off.st.water_refract);
    CHECK(off.st.water_reflect);
    for (const u32 x : {20u, 60u, 140u}) {
        CAPTURE(x);
        const Array<u32> rows = stripe_rows(f, x, level);
        REQUIRE(rows.size() > 20);
        const f64 s_on = column_stddev(on.px, x, rows), s_off = column_stddev(off.px, x, rows);
        MESSAGE("column " << x << ": luma spread " << s_on << " with the refraction, " << s_off << " without");
        CHECK(s_on > 20.0);
        CHECK(s_off < 6.0);
        // Calm water bends nothing: the bright stripes are on the rows the dry seabed has them on.
        const Array<bool> b_dry = bright_rows(dry.px, x, rows), b_on = bright_rows(on.px, x, rows);
        u32 agree = 0;
        for (usize i = 0; i < rows.size(); ++i) agree += b_dry[i] == b_on[i] ? 1u : 0u;
        MESSAGE("column " << x << ": " << agree << " of " << rows.size() << " rows agree with the dry seabed");
        CHECK(f64(agree) >= 0.9 * f64(rows.size()));
    }
    // Waves: the stripes move, each by no more than the uv cap (in rows, plus the stripe edge's pixel).
    WaterSurface wavy = calm;
    wavy.amplitude = 0.12f;
    const Shot moved = shoot(f, &wavy, stripes);
    const i32 reach = i32(std::ceil(kWaterRefractMaxUv * f32(kH))) + 1;
    u32 changed = 0, beyond = 0, total = 0;
    for (const u32 x : {20u, 60u, 140u}) {
        const Array<u32> rows = stripe_rows(f, x, level);
        const Array<bool> b_calm = bright_rows(on.px, x, rows), b_wavy = bright_rows(moved.px, x, rows);
        for (usize i = 0; i < rows.size(); ++i) {
            ++total;
            if (b_calm[i] != b_wavy[i]) ++changed;
            if (!b_wavy[i]) continue;
            bool near = false;
            for (usize j = 0; j < rows.size(); ++j)
                near = near || (b_calm[j] && std::abs(i32(rows[j]) - i32(rows[i])) <= reach);
            beyond += near ? 0u : 1u;
        }
    }
    MESSAGE("waves: " << changed << " of " << total << " rows change brightness class, " << beyond
                      << " bright rows farther than " << reach << " rows from a calm bright row");
    CHECK(changed > 0);
    CHECK(beyond == 0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("water optics: red dies first with depth, and deep water ends at the old body colour") {
    Fixture3D f;
    const vec3 seabed{-2.5f, 0.0f, 2.0f};  // on the grey slab, clear of the box
    Rgb px[2];
    const f32 levels[2] = {0.5f, 3.0f};
    for (u32 i = 0; i < 2; ++i) {
        const WaterSurface w = sea(levels[i], 0.0f);
        const Shot s = shoot(f, &w, {}, false, true);
        CHECK(s.st.water_refract);
        px[i] = f.at_world(seabed);
    }
    MESSAGE("seabed through 0.5 m: " << px[0].r << "," << px[0].g << "," << px[0].b << "; through 3 m: " << px[1].r
                                     << "," << px[1].g << "," << px[1].b);
    REQUIRE(px[0].r > 20);
    REQUIRE(px[0].b > 20);
    // the fraction of red that survives the extra depth is smaller than that of blue
    CHECK(f64(px[1].r) * f64(px[0].b) < f64(px[1].b) * f64(px[0].r));
    // A short absorption length (0.5 m) under 3 m of water: nothing of the seabed survives, the old body colour stays.
    WaterSurface deep = sea(3.0f, 0.0f);
    deep.depth_scale_m = 0.5f;
    shoot(f, &deep, {}, false, true);
    const Rgb with = f.at_world(seabed);
    shoot(f, &deep, {}, false, false);
    const Rgb without = f.at_world(seabed);
    MESSAGE("deep basin: " << with.r << "," << with.g << "," << with.b << " against the tint " << without.r << ","
                           << without.g << "," << without.b);
    CHECK(std::abs(with.r - without.r) <= 3);
    CHECK(std::abs(with.g - without.g) <= 3);
    CHECK(std::abs(with.b - without.b) <= 3);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("water optics: a post's dry part never shows in the water around it (no foreground leak)") {
    for (const u32 msaa : {4u, 1u}) {
        CAPTURE(msaa);
        Fixture3D f(msaa);
        upload_post(f);
        const auto post = [](Scene3D& s) { add_mesh(s, kPost); };
        const Shot dry = shoot(f, nullptr, post);
        // The dry frame's green silhouette, grown by 2 pixels (the half-size depth's edge).
        Array<bool> mask;
        mask.resize(usize(kW) * kH, false);
        u32 dry_green = 0;
        for (i32 y = 0; y < i32(kH); ++y)
            for (i32 x = 0; x < i32(kW); ++x) {
                if (!greenish(dry.px, (usize(y) * kW + usize(x)) * 4)) continue;
                ++dry_green;
                for (i32 dy = -2; dy <= 2; ++dy)
                    for (i32 dx = -2; dx <= 2; ++dx) {
                        const i32 nx = x + dx, ny = y + dy;
                        if (nx >= 0 && ny >= 0 && nx < i32(kW) && ny < i32(kH)) mask[usize(ny) * kW + usize(nx)] = true;
                    }
            }
        REQUIRE(dry_green > 40);
        for (const f32 amp : {0.0f, 0.3f}) {
            CAPTURE(amp);
            WaterSurface w = sea(0.6f, amp, 0.2f);
            w.wavelength = 3.0f;
            const Shot wet = shoot(f, &w, post, false, true);
            CHECK(wet.st.water_refract);
            u32 leaks = 0, inside = 0;
            for (usize p = 0; p < mask.size(); ++p) {
                if (!greenish(wet.px, p * 4)) continue;
                (mask[p] ? inside : leaks) += 1;
            }
            MESSAGE("green pixels: " << inside << " on the post, " << leaks << " in the water around it");
            CHECK(inside > 40);
            CHECK(leaks == 0);
        }
        CHECK(f.device->validation_error_count() == 0);
    }
}

TEST_CASE("water optics: glass above the water still shows the water behind it") {
    for (const u32 msaa : {4u, 1u}) {
        CAPTURE(msaa);
        Fixture3D f(msaa);
        mesh_fixture::Builder b;
        b.add_node(-1);
        mesh_fixture::Box g;
        g.center = {1.6f, 1.1f, 1.0f};
        g.half = {0.4f, 0.4f, 0.4f};
        g.color[0] = g.color[1] = g.color[2] = 0.5f;
        g.uvs = true;
        b.add_box(g);
        b.submeshes.back().material.roughness = 0.0f;
        asset::MaterialExt glass{};
        glass.transmission = 1.0f;
        b.materials.push_back(glass);
        REQUIRE(f.renderer.meshes().upload(kGlass, 1, b.build().span()).has_value());
        const auto with_glass = [](Scene3D& s) { add_mesh(s, kGlass); };
        const WaterSurface w = sea(0.6f, 0.2f);
        const Shot wet = shoot(f, &w, with_glass);
        CHECK(wet.st.water_refract);
        CHECK(wet.st.transmissive == 1);
        const Shot dry = shoot(f, nullptr, with_glass);
        // The glass's front face (its background below the horizon is the water, not the dry slab).
        const math::vec2 c = screen_of(f, vec3{1.6f, 1.0f, 1.4f});
        const usize i = (usize(c.y) * kW + usize(c.x)) * 4;
        MESSAGE("through the glass: " << int(wet.px[i]) << "," << int(wet.px[i + 1]) << "," << int(wet.px[i + 2])
                                      << "; without water " << int(dry.px[i]) << "," << int(dry.px[i + 1]) << ","
                                      << int(dry.px[i + 2]));
        CHECK(std::abs(int(dry.px[i + 2]) - int(dry.px[i])) <= 3);  // the grey slab without water
        CHECK(int(wet.px[i + 2]) > int(wet.px[i]) + 8);             // the water's blue-green through the glass
        CHECK(std::fabs(luma(wet.px, i) - luma(dry.px, i)) > 4.0);
        CHECK(f.device->validation_error_count() == 0);
    }
}

TEST_CASE("water optics: an emissive pillar is mirrored at the analytic position, and the waves widen its image") {
    for (const u32 msaa : {4u, 1u}) {
        CAPTURE(msaa);
        Fixture3D f(msaa);
        water_camera(f);
        upload_pillar(f);
        const auto pillar = [](Scene3D& s) { add_mesh(s, kPillar); };
        const f32 level = 0.3f;
        const WaterSurface calm = dark(sea(level, 0.0f));
        const Shot on = shoot(f, &calm, pillar, true, false);
        CHECK(on.st.water_reflect);
        const Shot sky_only = shoot(f, &calm, pillar, false, false);
        CHECK_FALSE(sky_only.st.water_reflect);
        // The mirror image of the pillar's front face lies below the waterline; its far end is the mirror of the top.
        const math::vec2 water_line = screen_of(f, vec3{kPillarX, level, kPillarFront});
        const math::vec2 top_mirror = screen_of(f, vec3{kPillarX, 2.0f * level - 2.5f, kPillarFront});
        const u32 x = u32(water_line.x);
        i32 lowest = -1;
        u32 green_on = 0, green_off = 0;
        for (u32 y = u32(water_line.y) + 1; y < kH; ++y) {
            const usize i = (usize(y) * kW + x) * 4;
            if (greenish(on.px, i)) {
                lowest = i32(y);
                ++green_on;
            }
            green_off += greenish(sky_only.px, i) ? 1u : 0u;
        }
        MESSAGE("mirror image down column " << x << ": " << green_on << " green rows (sky only: " << green_off
                                            << "), the lowest " << lowest << ", the analytic mirror of the top "
                                            << top_mirror.y);
        CHECK(green_off == 0);
        CHECK(green_on > 10);
        // Within 3 pixels: the image ends where the half-size depth's pillar ends, one half-size texel = 2 pixels
        // (measured 2.4 pixels short on llvmpipe, a sharp edge).
        CHECK(std::fabs(f32(lowest) + 0.5f - top_mirror.y) <= 3.0f);
        // Two runs agree.
        CHECK(same(shoot(f, &calm, pillar, true, false).px, on.px));
        // Waves: the image's side edges spread (pixels between 10 % and 90 % of the row's green excess across the
        // image, over rows in its middle, where the fresnel term hardly changes along a row).
        const auto edge_width = [&](const Array<u8>& px) {
            f64 total = 0.0;
            u32 rows = 0;
            for (u32 y = u32(water_line.y) + 4; y < u32(water_line.y) + 16 && y < kH; ++y) {
                f64 hi = 0.0;
                Array<f64> g;
                for (i32 cx = i32(x) - 14; cx <= i32(x) + 14; ++cx) {
                    const usize i = (usize(y) * kW + usize(cx)) * 4;
                    const f64 e = std::max(0.0, f64(px[i + 1]) - 0.5 * (f64(px[i]) + f64(px[i + 2])));
                    g.push_back(e);
                    hi = std::max(hi, e);
                }
                if (hi < 30.0) continue;
                u32 mid = 0;
                for (f64 e : g) mid += (e > 0.1 * hi && e < 0.9 * hi) ? 1u : 0u;
                total += f64(mid);
                ++rows;
            }
            return rows ? total / f64(rows) : 0.0;
        };
        f64 prev = -1.0;
        f64 widths[4];
        u32 k = 0;
        for (const f32 amp : {0.0f, 0.03f, 0.06f, 0.12f}) {
            WaterSurface w = calm;
            w.amplitude = amp;
            widths[k] = edge_width(shoot(f, &w, pillar, true, false).px);
            MESSAGE("amplitude " << amp << ": mean 10-90 % rows " << widths[k]);
            CHECK(widths[k] >= prev);
            prev = widths[k++];
        }
        CHECK(widths[3] > widths[0]);
        CHECK(f.device->validation_error_count() == 0);
    }
}

TEST_CASE("water optics: nothing above the water and a pillar under it reflect the sky bit for bit") {
    Fixture3D f;
    water_camera(f);
    upload_pillar(f);
    const auto pillar = [](Scene3D& s) { add_mesh(s, kPillar); };
    // Level 3: the box, the slab and the pillar (top 2.5 m) all under the water.
    const WaterSurface high = sea(3.0f, 0.1f);
    f.look_at = {0.0f, 3.5f, 0.0f};
    const Shot on = shoot(f, &high, pillar);
    CHECK(on.st.water_reflect);
    const Shot off = shoot(f, &high, pillar, false, true);
    CHECK(same(on.px, off.px));
    // No geometry at all besides the fixture's (under the water): the same.
    const Shot bare_on = shoot(f, &high), bare_off = shoot(f, &high, {}, false, true);
    CHECK(same(bare_on.px, bare_off.px));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("water optics: a pillar's reflection fades continuously as the camera turns it out of the frame") {
    Fixture3D f;
    water_camera(f);
    upload_pillar(f);
    const auto pillar = [](Scene3D& s) { add_mesh(s, kPillar); };
    const WaterSurface calm = dark(sea(0.3f, 0.0f));
    // The pillar's image leaves the frame's left edge as the camera yaws toward +x (turning right).
    Array<f64> flux;
    for (u32 i = 0; i <= 80; ++i) {
        f.yaw_deg = -14.0f - 0.25f * f32(i);
        const Shot s = shoot(f, &calm, pillar, true, false);
        const Shot sky = shoot(f, &calm, pillar, false, false);
        f64 sum = 0.0;
        for (usize p = 0; p < usize(kW) * kH; ++p) sum += std::max(0.0, f64(s.px[p * 4 + 1]) - f64(sky.px[p * 4 + 1]));
        flux.push_back(sum);
    }
    f.yaw_deg = 0.0f;
    const f64 top = *std::max_element(flux.begin(), flux.end());
    f64 step = 0.0;
    for (usize i = 1; i < flux.size(); ++i) step = std::max(step, std::fabs(flux[i] - flux[i - 1]));
    MESSAGE("reflected green over the turn: first " << flux.front() << ", last " << flux.back() << ", largest step "
                                                    << step << " of " << top);
    REQUIRE(top > 500.0);
    CHECK(flux.back() < 0.05 * top);
    CHECK(step < 0.15 * top);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("water optics: the Low look, no prepass and the camera under the level are untouched by the two cvars") {
    // The Low tier's look (amplitude and foam 0: no depth read).
    {
        Fixture3D f;
        water_camera(f);
        upload_pillar(f);
        const auto pillar = [](Scene3D& s) { add_mesh(s, kPillar); };
        const WaterSurface low = sea(0.3f, 0.0f, 0.0f);
        const Shot on = shoot(f, &low, pillar), off = shoot(f, &low, pillar, false, false);
        CHECK_FALSE(on.st.water_reflect);
        CHECK_FALSE(on.st.water_refract);
        CHECK(same(on.px, off.px));
        CHECK(f.device->validation_error_count() == 0);
    }
    // A renderer without the prepass pipelines (SSAO, SSR and contact shadows off).
    {
        Forward3DDesc d;
        d.msaa = 4;
        d.ssao = false;
        d.ssr = false;
        d.contact_shadows = false;
        Fixture3D f(4, true, true, 1.0f, &d);
        water_camera(f);
        upload_pillar(f);
        const auto pillar = [](Scene3D& s) { add_mesh(s, kPillar); };
        const WaterSurface w = sea(0.3f, 0.2f);
        const Shot on = shoot(f, &w, pillar), off = shoot(f, &w, pillar, false, false);
        CHECK_FALSE(on.st.water_reflect);
        CHECK_FALSE(on.st.water_refract);
        CHECK(same(on.px, off.px));
        CHECK(f.device->validation_error_count() == 0);
    }
    // The camera under the level (the underwater look's job).
    {
        Fixture3D f;
        upload_pillar(f);
        const auto pillar = [](Scene3D& s) { add_mesh(s, kPillar); };
        underwater_place_camera(f, {0.0f, 1.2f, 7.0f}, -10.0f);
        const WaterSurface w = sea(3.2f, 0.1f);
        const Shot on = shoot(f, &w, pillar), off = shoot(f, &w, pillar, false, false);
        CHECK_FALSE(on.st.water_reflect);
        CHECK_FALSE(on.st.water_refract);
        CHECK(same(on.px, off.px));
        CHECK(f.device->validation_error_count() == 0);
    }
}

TEST_CASE("water optics: each cvar alone runs its own effect, both on run both, and two runs agree") {
    Fixture3D f;
    water_camera(f);
    upload_pillar(f);
    const auto pillar = [](Scene3D& s) { add_mesh(s, kPillar); };
    const WaterSurface w = sea(0.3f, 0.1f);
    const Shot both = shoot(f, &w, pillar), refl = shoot(f, &w, pillar, true, false),
               refr = shoot(f, &w, pillar, false, true), none = shoot(f, &w, pillar, false, false);
    CHECK((both.st.water_reflect && both.st.water_refract));
    CHECK((refl.st.water_reflect && !refl.st.water_refract));
    CHECK((!refr.st.water_reflect && refr.st.water_refract));
    CHECK((!none.st.water_reflect && !none.st.water_refract));
    CHECK_FALSE(same(both.px, none.px));
    CHECK_FALSE(same(refl.px, none.px));
    CHECK_FALSE(same(refr.px, none.px));
    CHECK(same(shoot(f, &w, pillar).px, both.px));
    CHECK(f.device->validation_error_count() == 0);
}
