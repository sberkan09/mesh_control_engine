// M-LIGHTCOOKIE 40c to 40f against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: the cookie
// atlas bake (Forward3D::record_cookie_bake, kCookieBakeFS, one layer per distinct cookie texture with its mip chain
// packed beside it) and the spot cookie in the lit pass's local-light loop (MCE_GLSL_COOKIE, kMeshFS and kTerrainFS),
// sharing sampler bank 2 with the third decal texture. No cookie, -r.light_cookies 0, Forward3DDesc::light_cookies false
// or cookie_strength 0 draw the frame without cookies bit for bit; a cookie shapes its spot inside the cone only (a white
// texel is the plain light exactly, a black one no light at all, so its shadow taps are skipped); the uv convention is
// pinned by an asymmetric cookie against spot_cookie_matrix (the CPU reference); the roll turns the pattern; the edge
// fade darkens the border; the mips average a fine pattern at a distance; two lights with one texture share a layer and
// a 17th distinct texture (or a light past the tier's cap) draws plain; a cookie and a local shadow multiply; a terrain
// gets the cookie through the same body; a texture that is not loaded yet is the plain light until it is; an unchanged
// scene bakes nothing; a frame with a cookie draws decals from two textures; two views of different sizes in one command
// buffer each match the view alone; the Low tier's smaller atlas keeps the pattern with its caps. M-LIGHTCOOKIE 40e: the
// sun's cookie tiled over the plane across the sun (sun_cookie_uv the CPU reference: period, offset, roll), seam-free at
// the tile borders, multiplying the direct sun term only (black: the sun-less frame; with the cascades: a shadow stays
// dark), on a terrain, by distance through the mips, on the Low tier, in two views. 40f: the spot's and the sun's
// cookie in the volumetric fog's light injection (a window grid darkens the shafts' mullions; white or no cookie: the
// plain shafts bit for bit). One 160 x 120 frame per render.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <functional>

#include "fixture3d.h"
#include "mce/convar.h"
#include "mce/render/light_cookie.h"
#include "mce/render/quality.h"

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

bool same(const Array<u8>& a, const Array<u8>& b) { return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin()); }

struct CvarScope {  // sets a convar for the scope, resets it after
    ConVar* cv;
    CvarScope(const char* name, const char* value) : cv(ConVar::find(StringId(name))) {
        REQUIRE(cv != nullptr);
        REQUIRE(cv->set_string(value));
    }
    ~CvarScope() { cv->reset(); }
    CvarScope(const CvarScope&) = delete;
    CvarScope& operator=(const CvarScope&) = delete;
};

// A cookie texture of w x h texels, rgb from `texel(x, y)` (row 0 at v = 0), with its box mip chain down to 1 x 1
// (a cooked texture's shape), uploaded under a fresh key.
u32 upload_cookie(Fixture3D& f, u32 w, u32 h, const std::function<vec3(u32, u32)>& texel, bool mips = true) {
    Array<Array<f32>> levels;
    Array<f32> l0;
    l0.resize(usize(w) * h * 3);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const vec3 c = texel(x, y);
            l0[(usize(y) * w + x) * 3] = c.x;
            l0[(usize(y) * w + x) * 3 + 1] = c.y;
            l0[(usize(y) * w + x) * 3 + 2] = c.z;
        }
    levels.push_back(std::move(l0));
    u32 lw = w, lh = h;
    while (mips && (lw > 1 || lh > 1)) {
        const u32 nw = std::max(lw / 2, 1u), nh = std::max(lh / 2, 1u);
        Array<f32> n;
        n.resize(usize(nw) * nh * 3);
        const Array<f32>& p = levels.back();
        for (u32 y = 0; y < nh; ++y)
            for (u32 x = 0; x < nw; ++x)
                for (u32 c = 0; c < 3; ++c) {
                    f32 sum = 0.0f;
                    for (u32 dy = 0; dy < 2; ++dy)
                        for (u32 dx = 0; dx < 2; ++dx) {
                            const u32 sx = std::min(2 * x + dx, lw - 1), sy = std::min(2 * y + dy, lh - 1);
                            sum += p[(usize(sy) * lw + sx) * 3 + c];
                        }
                    n[(usize(y) * nw + x) * 3 + c] = sum * 0.25f;
                }
        levels.push_back(std::move(n));
        lw = nw;
        lh = nh;
    }
    Array<Array<u8>> bytes;
    Array<TextureMipUpload> ups;
    lw = w;
    lh = h;
    for (const Array<f32>& lv : levels) {
        Array<u8> b;
        for (usize i = 0; i < lv.size(); i += 3) {
            for (u32 c = 0; c < 3; ++c) b.push_back(static_cast<u8>(std::lround(std::clamp(lv[i + c], 0.0f, 1.0f) * 255.0f)));
            b.push_back(255);
        }
        bytes.push_back(std::move(b));
    }
    for (const Array<u8>& b : bytes) {
        ups.push_back(TextureMipUpload{lw, lh, std::as_bytes(b.span())});
        lw = std::max(lw / 2, 1u);
        lh = std::max(lh / 2, 1u);
    }
    TextureUpload up;
    up.mips = ups.span();
    const u32 key = f.renderer.textures().allocate_key();
    REQUIRE(f.renderer.textures().upload(key, 1, up).has_value());
    return key;
}

u32 solid_cookie(Fixture3D& f, vec3 c) {
    return upload_cookie(f, 4, 4, [c](u32, u32) { return c; });
}

// Bright where uv.x > 0.5 and uv.y > 0.5 (the corner away from row 0 and column 0), black elsewhere: pins both axes.
u32 corner_cookie(Fixture3D& f) {
    return upload_cookie(f, 64, 64, [](u32 x, u32 y) { return x >= 32 && y >= 32 ? vec3{1, 1, 1} : vec3{0, 0, 0}; });
}

// Black where uv.x < 0.5, white elsewhere.
u32 half_cookie(Fixture3D& f) {
    return upload_cookie(f, 64, 64, [](u32 x, u32) { return x >= 32 ? vec3{1, 1, 1} : vec3{0, 0, 0}; });
}

// The fixture's frame at night: no sun, a faint ambient, no sky, dark fog: the spot lights are what lights the ground.
void night(Scene3D& s) {
    no_sun(s);
    DirectionalLight sun = s.light();
    sun.ambient_sky = sun.ambient_ground = {0.03f, 0.03f, 0.05f};
    s.set_light(sun);
    Sky sky;
    sky.enabled = false;
    sky.specular_ambient = 0.0f;
    s.set_sky(sky);
    Fog fog;
    fog.color = {0.01f, 0.01f, 0.02f};
    s.set_fog(fog);
    PostFx post;  // no bloom or FXAA: a pixel is its own surface's light, so the probes compare exactly
    post.bloom = false;
    post.fxaa = false;
    s.set_post(post);
}

// The test spot: above the open ground right of the box, pointing down and a little away from the camera.
PointLight test_spot(vec3 pos = {2.2f, 3.2f, 0.6f}, u32 cookie = ~0u) {
    PointLight l;
    l.position = pos;
    l.direction = {0.0f, -1.0f, -0.2f};
    l.radius = 9.0f;
    l.intensity = 30.0f;
    l.spot_inner = math::radians(26.0f);
    l.spot_outer = math::radians(30.0f);
    l.cookie = cookie;
    return l;
}

struct Shot {
    Array<u8> px;
    Forward3DStats stats;
    rhi::FrameStats gpu;
};

Shot draw(Fixture3D& f, const std::function<void(Scene3D&)>& lights) {
    f.render(0.0f, false, [&](Scene3D& s) {
        night(s);
        if (lights) lights(s);
    });
    CHECK(f.device->validation_error_count() == 0);
    return {f.pixels, f.renderer.forward_3d().last_stats(), f.gpu};
}

// The cookie uv of world point p for light l (spot_cookie_matrix, the CPU reference the shader mirrors).
math::vec2 cookie_uv(const PointLight& l, vec3 p) {
    const math::mat4 m = spot_cookie_matrix(l.position, l.direction, l.spot_outer, l.cookie_roll, l.radius);
    const math::vec4 c = m * math::vec4(p, 1.0f);
    return {c.x / c.w * 0.5f + 0.5f, c.y / c.w * 0.5f + 0.5f};
}

// The ground point (y = 0) whose cookie uv is nearest `uv`, from a 2 cm grid over the slab; false when none is within
// 0.01 in uv (the uv falls off the slab, or the ray through it misses the ground).
bool find_ground_at_uv(const PointLight& l, math::vec2 uv, vec3& out, f32 y = 0.0f) {
    vec3 best{0, y, 0};
    f32 best_d = 1e9f;
    for (f32 z = -6.0f; z <= 6.0f; z += 0.02f)
        for (f32 x = -6.0f; x <= 6.0f; x += 0.02f) {
            const math::vec2 q = cookie_uv(l, {x, y, z});
            const f32 d = (q.x - uv.x) * (q.x - uv.x) + (q.y - uv.y) * (q.y - uv.y);
            if (d < best_d) {
                best_d = d;
                best = {x, y, z};
            }
        }
    out = best;
    return best_d < 1e-4f;
}

vec3 ground_at_uv(const PointLight& l, math::vec2 uv, f32 y = 0.0f) {
    vec3 p;
    REQUIRE(find_ground_at_uv(l, uv, p, y));
    return p;
}

bool same_at(const Fixture3D& f, const Array<u8>& a, const Array<u8>& b, vec3 p) {
    const math::vec4 c = f.view.view_proj * math::vec4(p, 1.0f);
    const f32 x = (c.x / c.w * 0.5f + 0.5f) * kW, y = (0.5f - c.y / c.w * 0.5f) * kH;
    REQUIRE(x >= 0);
    REQUIRE(y >= 0);
    REQUIRE(x < kW);
    REQUIRE(y < kH);
    const usize i = (usize(y) * kW + usize(x)) * 4;
    return a[i] == b[i] && a[i + 1] == b[i + 1] && a[i + 2] == b[i + 2];
}

int luma_at(const Fixture3D& f, const Array<u8>& px, vec3 p) {
    const math::vec4 c = f.view.view_proj * math::vec4(p, 1.0f);
    const f32 x = (c.x / c.w * 0.5f + 0.5f) * kW, y = (0.5f - c.y / c.w * 0.5f) * kH;
    REQUIRE(x >= 0);
    REQUIRE(y >= 0);
    REQUIRE(x < kW);
    REQUIRE(y < kH);
    const usize i = (usize(y) * kW + usize(x)) * 4;
    return (px[i] * 2 + px[i + 1] * 5 + px[i + 2]) / 8;
}

u32 changed_count(const Array<u8>& a, const Array<u8>& b) {
    u32 n = 0;
    for (usize i = 0; i < a.size(); i += 4) n += (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2]) ? 1u : 0u;
    return n;
}

}  // namespace

TEST_CASE("light cookie: no cookie, strength 0, -r.light_cookies 0 and Forward3DDesc::light_cookies false draw the plain frame") {
    Fixture3D f;
    const u32 key = half_cookie(f);
    const Shot plain = draw(f, [](Scene3D& s) { s.add_point_light(test_spot()); });
    CHECK(plain.stats.cookie_lights == 0);
    CHECK(plain.stats.cookie_layers == 0);
    const Shot cookied = draw(f, [&](Scene3D& s) { s.add_point_light(test_spot({2.2f, 3.2f, 0.6f}, key)); });
    REQUIRE(cookied.stats.cookie_lights == 1);
    CHECK(cookied.stats.cookie_layers == 1);
    CHECK_FALSE(same(cookied.px, plain.px));
    // strength 0: no record, no bank sharing, the plain frame.
    const Shot weak = draw(f, [&](Scene3D& s) {
        PointLight l = test_spot({2.2f, 3.2f, 0.6f}, key);
        l.cookie_strength = 0.0f;
        s.add_point_light(l);
    });
    CHECK(weak.stats.cookie_lights == 0);
    CHECK(same(weak.px, plain.px));
    // A cookie on a point light (spot_outer 0) is ignored.
    const Shot point_plain = draw(f, [](Scene3D& s) {
        PointLight l = test_spot();
        l.spot_outer = 0.0f;
        s.add_point_light(l);
    });
    const Shot point_cookied = draw(f, [&](Scene3D& s) {
        PointLight l = test_spot({2.2f, 3.2f, 0.6f}, key);
        l.spot_outer = 0.0f;
        s.add_point_light(l);
    });
    CHECK(point_cookied.stats.cookie_lights == 0);
    CHECK(same(point_cookied.px, point_plain.px));
    // r.light_cookies 0 while running: the renderer reads it every frame (and the kit extract through apply_quality).
    {
        CvarScope cv("r.light_cookies", "0");
        const Shot off = draw(f, [&](Scene3D& s) { s.add_point_light(test_spot({2.2f, 3.2f, 0.6f}, key)); });
        CHECK(off.stats.cookie_lights == 0);
        CHECK(same(off.px, plain.px));
        PointLight l = test_spot({2.2f, 3.2f, 0.6f}, key);
        apply_quality(quality_settings(QualityTier::High), l);
        CHECK(l.cookie == ~0u);
    }
    // Back on: the shaped frame again (the atlas survived the frames off).
    const Shot again = draw(f, [&](Scene3D& s) { s.add_point_light(test_spot({2.2f, 3.2f, 0.6f}, key)); });
    CHECK(same(again.px, cookied.px));
    CHECK(again.gpu.passes + 1 < cookied.gpu.passes);  // nothing re-baked: the layer still holds the texture
}

TEST_CASE("light cookie: Forward3DDesc::light_cookies false or -r.light_cookies 0 at init record nothing") {
    for (int k = 0; k < 2; ++k) {
        INFO((k == 0 ? "desc false" : "-r.light_cookies 0 at init"));
        Forward3DDesc d;
        d.msaa = 4;
        d.light_cookies = k != 0;
        ConVar* cv = ConVar::find(StringId("r.light_cookies"));
        REQUIRE(cv != nullptr);
        if (k == 1) REQUIRE(cv->set_string("0"));
        Fixture3D f(4, true, true, 1.0f, &d);
        const u32 key = half_cookie(f);
        const Shot plain = draw(f, [](Scene3D& s) { s.add_point_light(test_spot()); });
        const Shot cookied = draw(f, [&](Scene3D& s) { s.add_point_light(test_spot({2.2f, 3.2f, 0.6f}, key)); });
        CHECK(cookied.stats.cookie_lights == 0);
        CHECK(cookied.gpu.passes == plain.gpu.passes);  // no bake pass
        CHECK(same(cookied.px, plain.px));
        cv->reset();  // back on: the init-time read holds for the renderer's life
        const Shot later = draw(f, [&](Scene3D& s) { s.add_point_light(test_spot({2.2f, 3.2f, 0.6f}, key)); });
        CHECK(later.stats.cookie_lights == 0);
        CHECK(same(later.px, plain.px));
    }
}

TEST_CASE("light cookie: the pattern lands where spot_cookie_matrix puts it, only inside the cone") {
    Fixture3D f;
    const u32 key = corner_cookie(f);
    const PointLight lit = test_spot({2.2f, 3.2f, 0.6f}, key);
    const PointLight other = test_spot({-2.6f, 3.0f, 1.0f});  // a second, plain spot: unchanged
    const Shot none = draw(f, [&](Scene3D& s) { s.add_point_light(other); });
    const Shot plain = draw(f, [&](Scene3D& s) {
        s.add_point_light(test_spot());
        s.add_point_light(other);
    });
    const Shot shaped = draw(f, [&](Scene3D& s) {
        s.add_point_light(lit);
        s.add_point_light(other);
    });
    REQUIRE(shaped.stats.cookie_lights == 1);
    MESSAGE(changed_count(shaped.px, plain.px) << " pixels changed");
    // The bright corner (uv > 0.5 on both axes) is the plain spot exactly; the three dark ones get no light from it.
    const vec3 bright = ground_at_uv(lit, {0.72f, 0.72f});
    CHECK(same_at(f, shaped.px, plain.px, bright));
    CHECK(luma_at(f, plain.px, bright) > luma_at(f, none.px, bright) + 30);
    for (math::vec2 uv : {math::vec2{0.28f, 0.28f}, math::vec2{0.72f, 0.28f}, math::vec2{0.28f, 0.72f}}) {
        const vec3 p = ground_at_uv(lit, uv);
        INFO("dark corner uv " << uv.x << ", " << uv.y);
        CHECK(same_at(f, shaped.px, none.px, p));
        CHECK(luma_at(f, plain.px, p) > luma_at(f, none.px, p) + 30);
    }
    // Outside the cone and under the other spot: untouched.
    for (vec3 p : {vec3{-2.6f, 0.0f, 0.8f}, vec3{-1.5f, 0.0f, 2.6f}, vec3{0.0f, 0.5f, 0.5f}, vec3{4.5f, 0.0f, -4.0f}}) {
        INFO("outside probe " << p.x << ", " << p.z);
        CHECK(same_at(f, shaped.px, plain.px, p));
    }
    // Never brighter than the plain spot (a cookie only takes light away).
    u32 brighter = 0;
    for (usize i = 0; i < shaped.px.size(); i += 4) {
        const int a = (shaped.px[i] * 2 + shaped.px[i + 1] * 5 + shaped.px[i + 2]) / 8;
        const int b = (plain.px[i] * 2 + plain.px[i + 1] * 5 + plain.px[i + 2]) / 8;
        brighter += a > b ? 1u : 0u;
    }
    CHECK(brighter == 0);
}

TEST_CASE("light cookie: a black cookie is no light at all; a coloured one tints; strength 0.5 halves the contrast") {
    Fixture3D f;
    const u32 black = solid_cookie(f, {0, 0, 0});
    const u32 red = solid_cookie(f, {1.0f, 0.0f, 0.0f});
    const u32 half = half_cookie(f);
    const Shot none = draw(f, {});
    const Shot plain = draw(f, [](Scene3D& s) { s.add_point_light(test_spot()); });
    const Shot dark = draw(f, [&](Scene3D& s) { s.add_point_light(test_spot({2.2f, 3.2f, 0.6f}, black)); });
    REQUIRE(dark.stats.cookie_lights == 1);
    CHECK(same(dark.px, none.px));  // every texel black: the loop skips the light before anything else
    const Shot tinted = draw(f, [&](Scene3D& s) { s.add_point_light(test_spot({2.2f, 3.2f, 0.6f}, red)); });
    const PointLight probe_light = test_spot({2.2f, 3.2f, 0.6f}, red);
    const vec3 centre = ground_at_uv(probe_light, {0.5f, 0.5f});
    const Rgb t = f.at_world(centre);
    f.render(0.0f, false, [](Scene3D& s) {
        night(s);
        s.add_point_light(test_spot());
    });
    const Rgb p = f.at_world(centre);
    INFO("plain " << p.r << "," << p.g << "," << p.b << " tinted " << t.r << "," << t.g << "," << t.b);
    CHECK(std::abs(t.r - p.r) <= 3);  // the tonemapper's channel mix aside, red keeps the plain light
    CHECK(t.g < p.g / 4 + 3);
    CHECK(t.b < p.b / 4 + 3);
    (void)tinted;
    // Strength 0.5 over the half cookie: the black half keeps about half the light.
    const PointLight hl = test_spot({2.2f, 3.2f, 0.6f}, half);
    const Shot halfway = draw(f, [&](Scene3D& s) {
        PointLight l = hl;
        l.cookie_strength = 0.5f;
        s.add_point_light(l);
    });
    const vec3 dark_side = ground_at_uv(hl, {0.25f, 0.5f}), lit_side = ground_at_uv(hl, {0.75f, 0.5f});
    const int l_none = luma_at(f, none.px, dark_side), l_plain = luma_at(f, plain.px, dark_side),
              l_half = luma_at(f, halfway.px, dark_side);
    MESSAGE("dark side luma: no light " << l_none << ", plain " << l_plain << ", strength 0.5 " << l_half);
    CHECK(l_half > l_none + 5);
    CHECK(l_half < l_plain - 5);
    CHECK(same_at(f, halfway.px, plain.px, lit_side));
}

TEST_CASE("light cookie: cookie_roll turns the pattern about the cone axis") {
    Fixture3D f;
    const u32 key = corner_cookie(f);
    const Shot none = draw(f, {});
    const Shot plain = draw(f, [](Scene3D& s) { s.add_point_light(test_spot()); });
    PointLight rolled = test_spot({2.2f, 3.2f, 0.6f}, key);
    rolled.cookie_roll = math::radians(90.0f);
    const Shot r = draw(f, [&](Scene3D& s) { s.add_point_light(rolled); });
    REQUIRE(r.stats.cookie_lights == 1);
    // The CPU reference with the roll: the bright corner is where the rolled matrix says.
    const vec3 bright = ground_at_uv(rolled, {0.72f, 0.72f});
    CHECK(same_at(f, r.px, plain.px, bright));
    // The same ground point without the roll: a dark corner of the unrolled pattern, now dark or bright as rolled.
    const PointLight unrolled = test_spot({2.2f, 3.2f, 0.6f}, key);
    const vec3 was_bright = ground_at_uv(unrolled, {0.72f, 0.72f});
    const math::vec2 now = cookie_uv(rolled, was_bright);
    INFO("the unrolled bright corner sits at rolled uv " << now.x << ", " << now.y);
    CHECK(!(now.x > 0.5f && now.y > 0.5f));  // 90 degrees moved it to another corner
    CHECK(same_at(f, r.px, none.px, was_bright));
    // Rolled by 90 degrees, the bright point of the rolled frame is a dark one of the plain cookie.
    const math::vec2 back = cookie_uv(unrolled, bright);
    CHECK(!(back.x > 0.5f && back.y > 0.5f));
}

TEST_CASE("light cookie: cookie_edge fades the pattern toward the frustum border") {
    Fixture3D f;
    const u32 white = solid_cookie(f, {1, 1, 1});
    const Shot plain = draw(f, [](Scene3D& s) { s.add_point_light(test_spot()); });
    const Shot full = draw(f, [&](Scene3D& s) { s.add_point_light(test_spot({2.2f, 3.2f, 0.6f}, white)); });
    REQUIRE(full.stats.cookie_lights == 1);
    CHECK(same(full.px, plain.px));  // a white cookie is the plain light exactly
    const PointLight edged_l = [&] {
        PointLight l = test_spot({2.2f, 3.2f, 0.6f}, white);
        l.cookie_edge = 1.0f;
        return l;
    }();
    const Shot edged = draw(f, [&](Scene3D& s) { s.add_point_light(edged_l); });
    const vec3 centre = ground_at_uv(edged_l, {0.5f, 0.5f}), near_border = ground_at_uv(edged_l, {0.5f, 0.12f});
    CHECK(same_at(f, edged.px, plain.px, centre));  // smoothstep reaches 1 at half the frustum
    const int l_plain = luma_at(f, plain.px, near_border), l_edge = luma_at(f, edged.px, near_border);
    MESSAGE("near the border: plain " << l_plain << ", edge 1 " << l_edge);
    CHECK(l_edge < l_plain - 10);
}

TEST_CASE("light cookie: the mips average a fine pattern, a coarse one keeps its contrast") {
    Fixture3D f;
    // 256 x 256 texels: one-texel checker (fine) and 8 x 8 cells (coarse).
    const u32 fine = upload_cookie(f, 256, 256, [](u32 x, u32 y) { return ((x + y) & 1u) ? vec3{1, 1, 1} : vec3{0, 0, 0}; });
    const u32 coarse =
        upload_cookie(f, 256, 256, [](u32 x, u32 y) { return (((x / 32) + (y / 32)) & 1u) ? vec3{1, 1, 1} : vec3{0, 0, 0}; });
    const PointLight lf = test_spot({2.2f, 3.2f, 0.6f}, fine);
    const Shot none = draw(f, {});
    const Shot plain = draw(f, [](Scene3D& s) { s.add_point_light(test_spot()); });
    const Shot a = draw(f, [&](Scene3D& s) { s.add_point_light(lf); });
    const Shot b = draw(f, [&](Scene3D& s) { s.add_point_light(test_spot({2.2f, 3.2f, 0.6f}, coarse)); });
    REQUIRE(a.stats.cookie_lights == 1);
    // The luma spread over a grid of points inside the inner cone.
    auto spread = [&](const Shot& s) {
        int lo = 255, hi = 0;
        for (f32 v = 0.3f; v <= 0.701f; v += 0.05f)
            for (f32 u = 0.3f; u <= 0.701f; u += 0.05f) {
                const int l = luma_at(f, s.px, ground_at_uv(lf, {u, v}));
                lo = std::min(lo, l);
                hi = std::max(hi, l);
            }
        return hi - lo;
    };
    const int fine_spread = spread(a), coarse_spread = spread(b);
    MESSAGE("luma spread inside the cone: fine checker " << fine_spread << ", coarse " << coarse_spread);
    CHECK(coarse_spread > 40);
    CHECK(fine_spread * 4 < coarse_spread);
    // The fine checker's average lies between no light and the plain spot at every probe: a packed level read at the
    // wrong place (the cleared white beside it, or a neighbouring level) is the plain light or the raw checker.
    u32 between = 0, probes = 0;
    for (f32 v = 0.3f; v <= 0.701f; v += 0.1f)
        for (f32 u = 0.3f; u <= 0.701f; u += 0.1f) {
            const vec3 p = ground_at_uv(lf, {u, v});
            const int l = luma_at(f, a.px, p), l_none = luma_at(f, none.px, p), l_plain = luma_at(f, plain.px, p);
            INFO("uv " << u << ", " << v << ": none " << l_none << ", fine " << l << ", plain " << l_plain);
            CHECK(l > l_none + 10);
            CHECK(l < l_plain - 10);
            ++probes;
            between += (l > l_none + 10 && l < l_plain - 10) ? 1u : 0u;
        }
    CHECK(between == probes);
}

TEST_CASE("light cookie: two lights of one texture share a layer; past the tier's caps a light draws plain") {
    Fixture3D f;
    const QualitySettings q = quality_settings(quality_tier());
    const u32 key = half_cookie(f);
    const Shot two = draw(f, [&](Scene3D& s) {
        s.add_point_light(test_spot({2.2f, 3.2f, 0.6f}, key));
        s.add_point_light(test_spot({-2.6f, 3.0f, 1.0f}, key));
    });
    CHECK(two.stats.cookie_lights == 2);
    CHECK(two.stats.cookie_layers == 1);
    // One more distinct texture than the tier's layers (or lights): the cookies of the first fill the caps, the last
    // light draws plain. Each cookie is black, so the cookied lights add nothing.
    const u32 n = std::min(q.light_cookie_layers, q.light_cookie_lights) + 1;
    Array<u32> keys;
    for (u32 i = 0; i < n; ++i) keys.push_back(solid_cookie(f, {0, 0, 0}));
    auto grid_light = [&](u32 i, u32 cookie) {
        PointLight l;
        const u32 column = i % 7, row = i / 7;
        l.position = {-2.1f + 0.7f * f32(column), 2.0f, 1.4f - 0.7f * f32(row)};
        l.direction = {0.0f, -1.0f, 0.0f};
        l.radius = 3.0f;
        l.intensity = 6.0f;
        l.spot_inner = math::radians(8.0f);
        l.spot_outer = math::radians(12.0f);
        l.cookie = cookie;
        return l;
    };
    const Shot plain = draw(f, [&](Scene3D& s) {
        for (u32 i = 0; i < n; ++i) s.add_point_light(grid_light(i, ~0u));
    });
    const Shot capped = draw(f, [&](Scene3D& s) {
        for (u32 i = 0; i < n; ++i) s.add_point_light(grid_light(i, keys[i]));
    });
    CHECK(capped.stats.cookie_lights == n - 1);
    CHECK(capped.stats.cookie_layers == std::min(n - 1, q.light_cookie_layers));
    const vec3 last{grid_light(n - 1, ~0u).position.x, 0.0f, grid_light(n - 1, ~0u).position.z};
    const vec3 first{grid_light(0, ~0u).position.x, 0.0f, grid_light(0, ~0u).position.z};
    CHECK(same_at(f, capped.px, plain.px, last));  // the dropped light draws plain
    CHECK(luma_at(f, capped.px, first) < luma_at(f, plain.px, first) - 10);
}

TEST_CASE("light cookie: a cookie and the spot's local shadow multiply; the shadow budget is unchanged") {
    Fixture3D f;
    const u32 key = half_cookie(f);
    // A shadowed spot from the right, so the box's shadow falls on the ground at its left.
    auto spot = [&](u32 cookie) {
        PointLight l;
        l.position = {2.4f, 2.6f, 0.0f};
        l.direction = {-1.0f, -0.8f, 0.0f};
        l.radius = 10.0f;
        l.intensity = 30.0f;
        l.spot_inner = math::radians(30.0f);
        l.spot_outer = math::radians(36.0f);
        l.cast_shadows = true;
        l.cookie = cookie;
        return l;
    };
    auto scene = [&](u32 cookie) {
        return [=](Scene3D& s) {
            LocalShadowSettings ls;
            ls.tile = 256;
            s.set_local_shadows(ls);
            s.add_point_light(spot(cookie));
        };
    };
    const Shot none = draw(f, [](Scene3D& s) {
        LocalShadowSettings ls;
        ls.tile = 256;
        s.set_local_shadows(ls);
    });
    const Shot plain = draw(f, scene(~0u));
    const Shot shaped = draw(f, scene(key));
    REQUIRE(shaped.stats.cookie_lights == 1);
    CHECK(shaped.stats.local_shadow_tiles == plain.stats.local_shadow_tiles);
    CHECK(plain.stats.local_shadow_tiles == 1);
    // The box's shadow lies along the cone axis (the half cookie's seam), so a white cookie pins "shadowed under a
    // bright texel stays as dark as without the cookie": the whole frame is the shadowed plain spot, byte for byte.
    const u32 white = solid_cookie(f, {1, 1, 1});
    const Shot lit_white = draw(f, scene(white));
    REQUIRE(lit_white.stats.cookie_lights == 1);
    CHECK(same(lit_white.px, plain.px));
    const vec3 in_shadow{-1.3f, 0.0f, 0.0f}, in_light{-1.3f, 0.0f, 1.2f};
    CHECK(luma_at(f, plain.px, in_shadow) < luma_at(f, plain.px, in_light) - 30);  // the shadow is there to keep
    CHECK(luma_at(f, plain.px, in_light) > luma_at(f, none.px, in_light) + 30);
    const PointLight l = spot(key);
    // In the white half: the shadowed spot exactly, lit or in the box's shadow. The probes keep a quarter of the
    // frustum from the black edge: the ground is grazing here, so the lod reaches the coarse levels where the two halves
    // blur into each other.
    u32 white_checked = 0, black_checked = 0, shadowed = 0;
    for (f32 v = 0.1f; v <= 0.901f; v += 0.05f)
        for (f32 u = 0.1f; u <= 0.901f; u += 0.05f) {
            if (std::fabs(u - 0.5f) < 0.2f) continue;
            if ((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f) > 0.16f) continue;  // inside the inner cone
            vec3 p;
            if (!find_ground_at_uv(l, {u, v}, p)) continue;  // the upper part of the cone misses the slab
            INFO("probe uv " << u << ", " << v << " at " << p.x << ", " << p.z);
            if (p.x > -0.55f && p.x < 0.55f && p.z > -0.55f && p.z < 0.55f) continue;  // under the box
            if (p.x > -1.2f && p.x < 1.2f && p.z < 0.0f) continue;  // behind it: the pixel is the box's face
            const math::vec4 c = f.view.view_proj * math::vec4(p, 1.0f);
            if (std::fabs(c.x / c.w) > 0.98f || std::fabs(c.y / c.w) > 0.98f) continue;
            if (u > 0.5f) {
                CHECK(same_at(f, shaped.px, plain.px, p));
                ++white_checked;
                shadowed += luma_at(f, plain.px, p) < luma_at(f, none.px, p) + 5 ? 1u : 0u;
            } else {
                CHECK(same_at(f, shaped.px, none.px, p));  // a black texel: no light, shadowed or not
                ++black_checked;
            }
        }
    MESSAGE("white-half probes " << white_checked << " (in shadow " << shadowed << "), black-half " << black_checked);
    CHECK(white_checked > 4);
    CHECK(black_checked > 4);
}

TEST_CASE("light cookie: a terrain gets the cookie through the same lit body") {
    Fixture3D f;
    const u32 key = half_cookie(f);
    TerrainShading shading;  // one untextured layer: the white splat stand-in weighs it 1
    shading.layer_count = 1;
    shading.size = {12.0f, 12.0f};
    shading.splat_resolution = 2;
    const PointLight l = test_spot({2.2f, 3.2f, 0.6f}, key);
    auto with_terrain = [&](u32 cookie) {
        return [&, cookie](Scene3D& s) {
            MeshInstance t;
            t.mesh = 2;  // the slab, 2 cm above the fixture's ground
            t.world = math::translation({0.0f, 0.02f, 0.0f});
            t.terrain = &shading;
            s.add(t);
            s.add_point_light(test_spot({2.2f, 3.2f, 0.6f}, cookie));
        };
    };
    const Shot none = draw(f, [&](Scene3D& s) {
        MeshInstance t;
        t.mesh = 2;
        t.world = math::translation({0.0f, 0.02f, 0.0f});
        t.terrain = &shading;
        s.add(t);
    });
    const Shot plain = draw(f, with_terrain(~0u));
    const Shot shaped = draw(f, with_terrain(key));
    REQUIRE(shaped.stats.cookie_lights == 1);
    const vec3 dark = ground_at_uv(l, {0.25f, 0.5f}, 0.02f), lit = ground_at_uv(l, {0.75f, 0.5f}, 0.02f);
    CHECK(luma_at(f, plain.px, dark) > luma_at(f, none.px, dark) + 30);
    CHECK(same_at(f, shaped.px, none.px, dark));
    CHECK(same_at(f, shaped.px, plain.px, lit));
}

TEST_CASE("light cookie: a texture not loaded yet is the plain light until it loads; an unchanged scene bakes nothing") {
    Fixture3D f;
    const u32 late = f.renderer.textures().allocate_key();  // nothing uploaded under it yet
    const Shot none = draw(f, {});
    const Shot plain = draw(f, [](Scene3D& s) { s.add_point_light(test_spot()); });
    const Shot waiting = draw(f, [&](Scene3D& s) { s.add_point_light(test_spot({2.2f, 3.2f, 0.6f}, late)); });
    CHECK(waiting.stats.cookie_lights == 1);
    CHECK(same(waiting.px, plain.px));  // baked white
    // It loads (black): the next frame re-bakes the layer and the light goes dark.
    Array<u8> px;
    for (u32 i = 0; i < 16; ++i) {
        for (u32 c = 0; c < 3; ++c) px.push_back(0);
        px.push_back(255);
    }
    const TextureMipUpload mip{4, 4, std::as_bytes(px.span())};
    TextureUpload up;
    up.mips = Span<const TextureMipUpload>(&mip, 1);
    REQUIRE(f.renderer.textures().upload(late, 1, up).has_value());
    const Shot loaded = draw(f, [&](Scene3D& s) { s.add_point_light(test_spot({2.2f, 3.2f, 0.6f}, late)); });
    CHECK(same(loaded.px, none.px));
    // The same scene again: no bake pass (the levels of a layer are one pass each).
    const Shot still = draw(f, [&](Scene3D& s) { s.add_point_light(test_spot({2.2f, 3.2f, 0.6f}, late)); });
    CHECK(same(still.px, loaded.px));
    const QualitySettings q = quality_settings(quality_tier());
    u32 levels = 1;
    for (u32 s = 16; s < 2048 && s * 2 <= q.light_cookie_size; s *= 2) ++levels;
    levels += 4;  // 16 = 2^4: log2(S) + 1 levels
    MESSAGE("passes: baking frame " << loaded.gpu.passes << ", unchanged frame " << still.gpu.passes);
    CHECK(loaded.gpu.passes == still.gpu.passes + levels);
}

TEST_CASE("light cookie: a frame with a cookie draws decals from two textures, without one from three") {
    Fixture3D f;
    const u32 key = half_cookie(f);
    u32 tex[3];
    for (u32 i = 0; i < 3; ++i) tex[i] = solid_cookie(f, {0.2f * f32(i + 1), 0.5f, 0.9f});
    auto decals = [&](Scene3D& s) {
        for (u32 i = 0; i < 3; ++i) {
            Decal d;
            d.world = math::translation({-3.0f + 1.5f * f32(i), 0.0f, 2.0f}) * math::scaling({1.0f, 0.6f, 1.0f});
            d.texture = tex[i];
            s.add_decal(d);
        }
    };
    const Shot without = draw(f, [&](Scene3D& s) {
        decals(s);
        s.add_point_light(test_spot());
    });
    CHECK(without.stats.decals == 3);
    const Shot with = draw(f, [&](Scene3D& s) {
        decals(s);
        s.add_point_light(test_spot({2.2f, 3.2f, 0.6f}, key));
    });
    CHECK(with.stats.cookie_lights == 1);
    CHECK(with.stats.decals == 2);
    // The first two decals draw as before; the third's ground is the decal-free ground.
    const Shot bare = draw(f, [&](Scene3D& s) { s.add_point_light(test_spot({2.2f, 3.2f, 0.6f}, key)); });
    CHECK(same_at(f, with.px, without.px, {-3.0f, 0.0f, 2.0f}));
    CHECK(same_at(f, with.px, without.px, {-1.5f, 0.0f, 2.0f}));
    CHECK(same_at(f, with.px, bare.px, {0.0f, 0.0f, 2.0f}));
    CHECK_FALSE(same_at(f, without.px, bare.px, {0.0f, 0.0f, 2.0f}));
}

TEST_CASE("light cookie: two views of different sizes in one command buffer each match the view drawn alone") {
    Fixture3D f;
    const u32 key = corner_cookie(f);
    const u32 key2 = half_cookie(f);
    constexpr u32 kSmallW = 96, kSmallH = 72;
    rhi::TextureDesc td;
    td.size = {kSmallW, kSmallH};
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.small";
    const rhi::TextureHandle small = f.device->create_texture(td);
    // The big view has the corner cookie, the small one both cookies (a second layer baked by the later view).
    auto record = [&](rhi::CommandBuffer& cmd, rhi::TextureHandle target, rhi::Extent2D size, u32 slot, bool first) {
        const Camera3D cam = orbit_camera({0, 0.5f, 0}, 0.0f, math::radians(35.0f), 7.0f);
        Scene3D scene(f.renderer.meshes(), mem::heap());
        scene.begin(cam, size);
        night(scene);
        MeshInstance ground;
        ground.mesh = 2;
        scene.add(ground);
        MeshInstance box;
        box.mesh = 1;
        scene.add(box);
        scene.add_point_light(test_spot({2.2f, 3.2f, 0.6f}, size.width == kW ? key : key2));
        if (size.width != kW) scene.add_point_light(test_spot({-2.6f, 3.0f, 1.0f}, key));
        scene.build();
        Forward3D::ViewOutput out;
        out.slot = slot;
        out.first = first;
        f.renderer.record_3d(cmd, scene, target, out);
        CHECK(f.renderer.forward_3d().last_stats().cookie_lights == (size.width == kW ? 1u : 2u));
    };
    Array<u8> big_alone, small_alone, big, small_px;
    big_alone.resize(usize(kW) * kH * 4);
    big.resize(usize(kW) * kH * 4);
    small_alone.resize(usize(kSmallW) * kSmallH * 4);
    small_px.resize(usize(kSmallW) * kSmallH * 4);
    for (int k = 0; k < 2; ++k) {
        f.device->begin_frame(f.frame++);
        rhi::CommandBuffer cmd;
        if (k == 0)
            record(cmd, f.target, {kW, kH}, 1, true);
        else
            record(cmd, small, {kSmallW, kSmallH}, 2, true);
        f.device->submit(cmd);
        if (k == 0)
            REQUIRE(f.device->read_pixels(f.target, rhi::Rect2D{0, 0, kW, kH}, big_alone.span()).has_value());
        else
            REQUIRE(f.device->read_pixels(small, rhi::Rect2D{0, 0, kSmallW, kSmallH}, small_alone.span()).has_value());
        f.device->end_frame();
    }
    for (int frame = 0; frame < 3; ++frame) {
        f.device->begin_frame(f.frame++);
        rhi::CommandBuffer cmd;
        const bool big_first = frame != 1;
        if (big_first) {
            record(cmd, f.target, {kW, kH}, 3, true);
            record(cmd, small, {kSmallW, kSmallH}, 4, false);
        } else {
            record(cmd, small, {kSmallW, kSmallH}, 4, true);
            record(cmd, f.target, {kW, kH}, 3, false);
        }
        f.device->submit(cmd);
        REQUIRE(f.device->read_pixels(f.target, rhi::Rect2D{0, 0, kW, kH}, big.span()).has_value());
        REQUIRE(f.device->read_pixels(small, rhi::Rect2D{0, 0, kSmallW, kSmallH}, small_px.span()).has_value());
        f.device->end_frame();
        INFO("frame " << frame << (big_first ? ", big view first" : ", small view first"));
        CHECK(f.device->validation_error_count() == 0);
        CHECK(same(big, big_alone));
        CHECK(same(small_px, small_alone));
    }
    f.device->destroy_texture(small);
}

TEST_CASE("light cookie: a non-power-of-two source does not bleed its opposite edge into the border") {
    Fixture3D f;
    // 300 x 300 (mips 150, 75, 37, ...): black where x < 150, white elsewhere. The bake's fractional lod mixes two source
    // mips through the repeat sampler; a clamp from the finer one lets the coarser wrap the white edge onto the black one.
    const u32 key = upload_cookie(f, 300, 300, [](u32 x, u32) { return x >= 150 ? vec3{1, 1, 1} : vec3{0, 0, 0}; });
    // Low over the ground with a wide, hard cone: a mid distance where the lit pass reads the coarse packed levels.
    auto spot = [&](u32 cookie) {
        PointLight l;
        l.position = {1.8f, 0.9f, 0.8f};
        l.direction = {0.0f, -1.0f, 0.0f};
        l.radius = 6.0f;
        l.intensity = 12.0f;
        l.spot_inner = math::radians(41.0f);
        l.spot_outer = math::radians(42.0f);
        l.cookie = cookie;
        return l;
    };
    const Shot none = draw(f, {});
    const Shot plain = draw(f, [&](Scene3D& s) { s.add_point_light(spot(~0u)); });
    const Shot shaped = draw(f, [&](Scene3D& s) { s.add_point_light(spot(key)); });
    REQUIRE(shaped.stats.cookie_lights == 1);
    const PointLight l = spot(key);
    u32 lit_probes = 0;
    for (f32 u = 0.04f; u <= 0.161f; u += 0.04f) {
        vec3 p;
        if (!find_ground_at_uv(l, {u, 0.5f}, p)) continue;
        INFO("black border probe uv " << u << " at " << p.x << ", " << p.z);
        if (luma_at(f, plain.px, p) <= luma_at(f, none.px, p) + 20) continue;  // outside the lit cone
        ++lit_probes;
        CHECK(same_at(f, shaped.px, none.px, p));  // no glow from the white edge across the wrap
    }
    for (f32 u = 0.84f; u <= 0.961f; u += 0.04f) {
        vec3 p;
        if (!find_ground_at_uv(l, {u, 0.5f}, p)) continue;
        INFO("white border probe uv " << u << " at " << p.x << ", " << p.z);
        if (luma_at(f, plain.px, p) <= luma_at(f, none.px, p) + 20) continue;
        ++lit_probes;
        CHECK(same_at(f, shaped.px, plain.px, p));  // no shade from the black edge either
    }
    CHECK(lit_probes >= 4);
}

TEST_CASE("light cookie: a later view recreates the atlas when the frame's first view has no cookie") {
    Fixture3D f;
    Array<u32> keys;
    for (u32 i = 0; i < 6; ++i) keys.push_back(i % 2 ? half_cookie(f) : corner_cookie(f));
    auto record = [&](rhi::CommandBuffer& cmd, bool cookies, bool first, u32 slot) {
        const Camera3D cam = orbit_camera({0, 0.5f, 0}, 0.0f, math::radians(35.0f), 7.0f);
        Scene3D scene(f.renderer.meshes(), mem::heap());
        scene.begin(cam, {kW, kH});
        night(scene);
        MeshInstance ground;
        ground.mesh = 2;
        scene.add(ground);
        if (cookies)
            for (u32 i = 0; i < 6; ++i) {
                PointLight l = test_spot({-2.5f + 1.0f * f32(i), 3.0f, 0.5f}, keys[i]);
                l.spot_outer = math::radians(12.0f);
                l.spot_inner = math::radians(10.0f);
                scene.add_point_light(l);
            }
        scene.build();
        Forward3D::ViewOutput out;
        out.slot = slot;
        out.first = first;
        f.renderer.record_3d(cmd, scene, f.target, out);
        return f.renderer.forward_3d().last_stats();
    };
    // Low: a 4-layer atlas from a frame with cookies.
    {
        CvarScope q("r.quality", "low");
        f.device->begin_frame(f.frame++);
        rhi::CommandBuffer cmd;
        const Forward3DStats st = record(cmd, true, true, 1);
        f.device->submit(cmd);
        f.device->end_frame();
        CHECK(st.cookie_layers == quality_settings(QualityTier::Low).light_cookie_layers);
    }
    // High: the frame's first view has no cookie, the second has six textures. The second recreates the atlas at the
    // High size and layer count (nothing of the frame used it yet), so all six draw with their cookies.
    {
        CvarScope q("r.quality", "high");
        for (int frame = 0; frame < 2; ++frame) {
            f.device->begin_frame(f.frame++);
            rhi::CommandBuffer cmd;
            const Forward3DStats a = record(cmd, false, true, 2);
            const Forward3DStats b = record(cmd, true, false, 3);
            f.device->submit(cmd);
            f.device->end_frame();
            INFO("frame " << frame);
            CHECK(f.device->validation_error_count() == 0);
            CHECK(a.cookie_lights == 0);
            CHECK(b.cookie_lights == 6);
            CHECK(b.cookie_layers == 6);
        }
    }
}

TEST_CASE("light cookie: the Low tier's smaller atlas keeps the pattern within its caps") {
    Fixture3D f;
    const u32 key = corner_cookie(f);
    const PointLight lit = test_spot({2.2f, 3.2f, 0.6f}, key);
    Shot low, high, none, plain;
    none = draw(f, {});
    plain = draw(f, [](Scene3D& s) { s.add_point_light(test_spot()); });
    {
        CvarScope q("r.quality", "high");
        high = draw(f, [&](Scene3D& s) { s.add_point_light(lit); });
    }
    {
        CvarScope q("r.quality", "low");
        low = draw(f, [&](Scene3D& s) { s.add_point_light(lit); });
        // Low: 4 layers and 4 cookied lights; a fifth distinct texture draws plain.
        Array<u32> keys;
        for (u32 i = 0; i < 5; ++i) keys.push_back(solid_cookie(f, {0, 0, 0}));
        const Shot capped = draw(f, [&](Scene3D& s) {
            for (u32 i = 0; i < 5; ++i) {
                PointLight l = test_spot({-3.0f + 1.5f * f32(i), 3.0f, 1.0f}, keys[i]);
                l.spot_outer = math::radians(10.0f);
                l.spot_inner = math::radians(8.0f);
                s.add_point_light(l);
            }
        });
        CHECK(capped.stats.cookie_lights == quality_settings(QualityTier::Low).light_cookie_lights);
        CHECK(capped.stats.cookie_layers == quality_settings(QualityTier::Low).light_cookie_layers);
    }
    REQUIRE(low.stats.cookie_lights == 1);
    REQUIRE(high.stats.cookie_lights == 1);
    CHECK(quality_settings(QualityTier::Low).light_cookie_size < quality_settings(QualityTier::High).light_cookie_size);
    // The pattern holds on both: the bright corner is the plain spot, a dark one no light.
    const vec3 bright = ground_at_uv(lit, {0.72f, 0.72f}), dark = ground_at_uv(lit, {0.28f, 0.28f});
    CHECK(same_at(f, low.px, plain.px, bright));
    CHECK(same_at(f, low.px, none.px, dark));
    CHECK(same_at(f, high.px, plain.px, bright));
    CHECK(same_at(f, high.px, none.px, dark));
    // (Low and High draw this 64-texel cookie alike: the atlas levels at or below the source's size hold the source's
    // own texels on either tier, and the lod here reaches them.)
}

// ---- M-LIGHTCOOKIE 40e: the sun's cookie (sun_cookie() in the sun term of MCE_GLSL_MESH_FS) ----------------------------

namespace {

constexpr vec3 kSunDown{0.0f, -1.0f, 0.0f};  // straight down: the plane's axes are then world x and z (+Z up hint)

SunCookie sun_cookie_key(u32 key, f32 size = 2.0f) {
    SunCookie c;
    c.texture = key;
    c.size = size;
    return c;
}

// Daylight from `dir` (no sky, a faint ambient, no bloom or FXAA, so a pixel is its own surface's light); intensity 0 is
// the frame without the sun's direct term (the ambient is the same: it does not depend on the sun's intensity).
void day(Scene3D& s, const SunCookie& cookie, f32 intensity, vec3 dir, bool shadows) {
    DirectionalLight sun;
    sun.direction = dir;
    sun.intensity = intensity;
    sun.shadows = shadows;
    sun.ambient_sky = sun.ambient_ground = {0.05f, 0.05f, 0.06f};
    sun.cookie = cookie;
    s.set_light(sun);
    Sky sky;
    sky.enabled = false;
    sky.specular_ambient = 0.0f;
    s.set_sky(sky);
    Fog fog;
    fog.color = {0.02f, 0.02f, 0.03f};
    s.set_fog(fog);
    PostFx post;
    post.bloom = false;
    post.fxaa = false;
    s.set_post(post);
}

Shot draw_sun(Fixture3D& f, const SunCookie& cookie, f32 intensity = 1.5f, vec3 dir = kSunDown, bool shadows = false,
              const std::function<void(Scene3D&)>& more = {}) {
    f.render(0.0f, shadows, [&](Scene3D& s) {
        day(s, cookie, intensity, dir, shadows);
        if (more) more(s);
    });
    CHECK(f.device->validation_error_count() == 0);
    return {f.pixels, f.renderer.forward_3d().last_stats(), f.gpu};
}

f32 fract01(f32 x) { return x - std::floor(x); }

// A ground point the camera sees (inside the frame's border, not on or behind the box). `shift`: where the fixture's
// world_offset put the ground and the box.
bool visible_ground(const Fixture3D& f, vec3 p, vec3 shift = {}) {
    if (std::fabs(p.x) < 1.0f && p.z < 0.8f) return false;
    const math::vec4 c = f.view.view_proj * math::vec4(p + shift, 1.0f);
    return c.w > 0.0f && std::fabs(c.x / c.w) < 0.96f && std::fabs(c.y / c.w) < 0.96f;
}

struct PatternCount {
    u32 bright = 0, dark = 0;
};

// The corner cookie (bright where u and v are both above 0.5) over the ground, against sun_cookie_uv (the CPU
// reference the shader mirrors): a probe a tenth of a repeat inside a bright cell is the plain sun exactly, one inside a
// dark cell the frame without the sun's direct term exactly (the probes keep to the nearer ground, where the mips'
// blur stays inside that margin).
// `pixel_shift` moves the probe in the frame (the fixture's world_offset), `world_shift` in the world the pattern lives in
// (a floating-origin rebase, or the same world_offset).
PatternCount check_corner(const Fixture3D& f, const Shot& shaped, const Shot& plain, const Shot& none, vec3 dir,
                          const SunCookie& c, f32 y = 0.0f, vec3 pixel_shift = {}, vec3 world_shift = {}) {
    PatternCount n;
    for (f32 z = -3.6f; z <= 5.0f; z += 0.1f)  // nearer than the far rows, whose mips blur past the cells' margin
        for (f32 x = -5.0f; x <= 5.0f; x += 0.1f) {
            const vec3 p{x, y, z};
            if (!visible_ground(f, p, pixel_shift)) continue;
            const math::vec2 uv = sun_cookie_uv(p + world_shift, dir, c);
            const f32 u = fract01(uv.x), v = fract01(uv.y);
            auto in = [](f32 t, f32 a, f32 b) { return t >= a && t <= b; };
            INFO("probe " << x << ", " << z << " at cookie uv " << u << ", " << v);
            if (in(u, 0.6f, 0.9f) && in(v, 0.6f, 0.9f)) {
                CHECK(same_at(f, shaped.px, plain.px, p + pixel_shift));
                ++n.bright;
            } else if ((in(u, 0.1f, 0.4f) && (in(v, 0.1f, 0.4f) || in(v, 0.6f, 0.9f))) || (in(v, 0.1f, 0.4f) && in(u, 0.6f, 0.9f))) {
                CHECK(same_at(f, shaped.px, none.px, p + pixel_shift));
                ++n.dark;
            }
        }
    return n;
}

// A 64 x 64 window grid: 3 x 3 panes, 6-texel mullions and frame (cookie_spot_window's image).
u32 window_cookie(Fixture3D& f) {
    return upload_cookie(f, 64, 64, [](u32 x, u32 y) {
        const bool bar = x % 21 < 6 || y % 21 < 6 || x >= 63 - 6 || y >= 63 - 6;
        return bar ? vec3{0, 0, 0} : vec3{1, 1, 1};
    });
}

}  // namespace

TEST_CASE("sun cookie: no cookie, strength 0, -r.light_cookies 0 and a white cookie draw the plain sun") {
    Fixture3D f;
    const u32 key = corner_cookie(f);
    const u32 white = solid_cookie(f, {1, 1, 1});
    const Shot plain = draw_sun(f, SunCookie{});
    CHECK_FALSE(plain.stats.sun_cookie);
    CHECK(plain.stats.cookie_layers == 0);
    const Shot shaped = draw_sun(f, sun_cookie_key(key));
    CHECK(shaped.stats.sun_cookie);
    CHECK(shaped.stats.cookie_layers == 1);
    CHECK(shaped.stats.cookie_lights == 0);  // the sun takes a layer, not a light record
    CHECK_FALSE(same(shaped.px, plain.px));
    SunCookie weak = sun_cookie_key(key);
    weak.strength = 0.0f;
    const Shot w = draw_sun(f, weak);
    CHECK_FALSE(w.stats.sun_cookie);
    CHECK(same(w.px, plain.px));
    {
        CvarScope cv("r.light_cookies", "0");
        const Shot off = draw_sun(f, sun_cookie_key(key));
        CHECK_FALSE(off.stats.sun_cookie);
        CHECK(same(off.px, plain.px));
        SunCookie c = sun_cookie_key(key);
        apply_quality(quality_settings(QualityTier::High), c);
        CHECK(c.texture == ~0u);
    }
    const Shot lit_white = draw_sun(f, sun_cookie_key(white));
    CHECK(lit_white.stats.sun_cookie);
    CHECK(same(lit_white.px, plain.px));  // a white texel is the plain sun bit for bit
    // Strength 0.5: a dark cell keeps part of the sun.
    SunCookie half = sun_cookie_key(key);
    half.strength = 0.5f;
    const Shot h = draw_sun(f, half);
    const Shot none = draw_sun(f, SunCookie{}, 0.0f);
    const vec3 dark{2.5f, 0.0f, -1.5f};  // uv (1.25, -0.75): a dark cell
    REQUIRE(fract01(sun_cookie_uv(dark, kSunDown, half).x) < 0.5f);
    CHECK(luma_at(f, h.px, dark) > luma_at(f, none.px, dark) + 10);
    CHECK(luma_at(f, h.px, dark) < luma_at(f, plain.px, dark) - 10);
}

TEST_CASE("sun cookie: the pattern tiles with the period `size`; the offset shifts and the roll turns it") {
    Fixture3D f;
    const u32 key = corner_cookie(f);
    const Shot plain = draw_sun(f, SunCookie{});
    const Shot none = draw_sun(f, SunCookie{}, 0.0f);
    REQUIRE(luma_at(f, plain.px, {2.5f, 0.0f, -1.5f}) > luma_at(f, none.px, {2.5f, 0.0f, -1.5f}) + 40);
    for (f32 size : {2.0f, 3.0f}) {
        INFO("size " << size);
        const SunCookie c = sun_cookie_key(key, size);
        const Shot s = draw_sun(f, c);
        const PatternCount n = check_corner(f, s, plain, none, kSunDown, c);
        MESSAGE("size " << size << ": " << n.bright << " bright and " << n.dark << " dark probes");
        CHECK(n.bright > 20);
        CHECK(n.dark > 40);
        // Pixel periodicity: a probe and the one a whole repeat along x (the plane's right) draw alike.
        u32 pairs = 0;
        for (f32 z = -5.0f; z <= 2.6f; z += 0.3f)
            for (f32 x = -4.0f; x + size <= 4.0f; x += 0.35f) {
                const vec3 p{x, 0.0f, z}, q{x + size, 0.0f, z};
                if (!visible_ground(f, p) || !visible_ground(f, q)) continue;
                const f32 u = fract01(sun_cookie_uv(p, kSunDown, c).x), v = fract01(sun_cookie_uv(p, kSunDown, c).y);
                if (std::fabs(u - 0.5f) < 0.1f || u < 0.1f || u > 0.9f || std::fabs(v - 0.5f) < 0.1f || v < 0.1f || v > 0.9f)
                    continue;
                INFO("pair " << x << ", " << z);
                CHECK(same_at(f, s.px, plain.px, p) == same_at(f, s.px, plain.px, q));
                CHECK(same_at(f, s.px, none.px, p) == same_at(f, s.px, none.px, q));
                ++pairs;
            }
        CHECK(pairs > 10);
    }
    // The offset (metres in the plane): the pattern follows sun_cookie_uv; a whole repeat of offset is the same frame.
    SunCookie moved = sun_cookie_key(key);
    moved.offset = {0.75f, 0.25f};
    const Shot m = draw_sun(f, moved);
    const PatternCount nm = check_corner(f, m, plain, none, kSunDown, moved);
    CHECK(nm.bright > 20);
    CHECK(nm.dark > 40);
    CHECK_FALSE(same(m.px, draw_sun(f, sun_cookie_key(key)).px));
    SunCookie whole = moved;
    whole.offset = {0.75f + 2.0f, 0.25f - 4.0f};
    CHECK(same(draw_sun(f, whole).px, m.px));
    // The roll turns the pattern in the plane.
    SunCookie rolled = sun_cookie_key(key);
    rolled.roll = math::radians(30.0f);
    const Shot r = draw_sun(f, rolled);
    const PatternCount nr = check_corner(f, r, plain, none, kSunDown, rolled);
    CHECK(nr.bright > 20);
    CHECK(nr.dark > 40);
    CHECK_FALSE(same(r.px, draw_sun(f, sun_cookie_key(key)).px));
}

TEST_CASE("sun cookie: a low sun carries the pattern to the ground; far from the origin and across a rebase it stays put") {
    const SunCookie c = sun_cookie_key(0u, 3.0f);  // the key is set per fixture below
    const vec3 dir = math::normalize(vec3{-0.3f, -1.0f, -0.2f});
    const vec3 far{1000.5f, 3.25f, -2000.25f};  // not a whole number of repeats on either axis
    // The camera, the ground and the box moved far from the origin (the fixture's world_offset): the shader's point is
    // relative to the camera, whose absolute ground point the CPU folds into the offset.
    {
        Fixture3D f;
        SunCookie ck = c;
        ck.texture = corner_cookie(f);
        f.world_offset = far;
        const Shot plain = draw_sun(f, SunCookie{}, 1.5f, dir);
        const Shot none = draw_sun(f, SunCookie{}, 0.0f, dir);
        const Shot shaped = draw_sun(f, ck, 1.5f, dir);
        REQUIRE(shaped.stats.sun_cookie);
        const PatternCount n = check_corner(f, shaped, plain, none, dir, ck, 0.0f, far, far);
        MESSAGE("far from the origin: " << n.bright << " bright and " << n.dark << " dark probes");
        CHECK(n.bright > 20);
        CHECK(n.dark > 40);
    }
    // A floating-origin rebase by `far`: the scene is drawn near the origin, the pattern is that of the world point.
    {
        Fixture3D f;
        SunCookie ck = c;
        ck.texture = corner_cookie(f);
        const Shot shaped = draw_sun(f, ck, 1.5f, dir, false, [&](Scene3D& s) { s.set_origin_shift(far); });
        REQUIRE(shaped.stats.sun_cookie);
        const Shot plain = draw_sun(f, SunCookie{}, 1.5f, dir);
        const Shot none = draw_sun(f, SunCookie{}, 0.0f, dir);
        const PatternCount n = check_corner(f, shaped, plain, none, dir, ck, 0.0f, {}, far);
        MESSAGE("after a rebase: " << n.bright << " bright and " << n.dark << " dark probes");
        CHECK(n.bright > 20);
        CHECK(n.dark > 40);
        // The same frame again (the origin kept): unchanged.
        CHECK(same(draw_sun(f, ck, 1.5f, dir).px, shaped.px));
    }
}

TEST_CASE("sun cookie: no seam at the tile borders (the wrapped read of a packed level, M-LIGHTCOOKIE 40e (b))") {
    Fixture3D f;
    // A seamless 8 x 8 pattern of strongly contrasted values, and the same image shifted by half its size on both axes.
    // With the shifted image and an offset of minus half a repeat the pattern on the ground is the same, but the first
    // frame's tile borders lie where the second has tile centres: any seam (a clamp in the bake or in the read) differs
    // by a large part of a neighbouring texel's difference (tens of levels here).
    auto value = [](u32 x, u32 y) {
        const f32 a = 6.2831853f * f32(x) / 8.0f, b = 6.2831853f * f32(y) / 8.0f;
        const f32 g = std::clamp(0.5f + 0.9f * (std::cos(a) * std::cos(2.0f * b) + 0.6f * std::sin(2.0f * a + b)), 0.0f, 1.0f);
        return vec3{g, 0.8f * g + 0.1f, 1.0f - 0.7f * g};
    };
    const u32 a = upload_cookie(f, 8, 8, value);
    const u32 b = upload_cookie(f, 8, 8, [&](u32 x, u32 y) { return value((x + 4) % 8, (y + 4) % 8); });
    const SunCookie ca = sun_cookie_key(a, 3.0f);
    SunCookie cb = sun_cookie_key(b, 3.0f);
    cb.offset = {-1.5f, -1.5f};
    const Shot sa = draw_sun(f, ca);
    const Shot sb = draw_sun(f, cb);
    const Shot plain = draw_sun(f, SunCookie{});
    REQUIRE(sa.stats.sun_cookie);
    CHECK_FALSE(same(sa.px, plain.px));
    int worst = 0;
    u32 differing = 0;
    for (usize i = 0; i < sa.px.size(); i += 4)
        for (u32 c = 0; c < 3; ++c) {
            const int d = std::abs(int(sa.px[i + c]) - int(sb.px[i + c]));
            worst = std::max(worst, d);
            differing += d > 0 ? 1u : 0u;
        }
    MESSAGE("half-shifted image: worst channel difference " << worst << ", " << differing << " channels differ");
    // What remains is the hardware bilinear's weight quantization (8 bits of sub-texel precision on llvmpipe and lavapipe,
    // the minimum GL and Vulkan allow) against the f32 rounding of the two offsets: at most 1/256 of a neighbouring
    // texel's difference, a level or two after the light and the tonemapper, on few pixels; never a stripe.
    CHECK(worst <= 2);
    CHECK(differing * 20 < u32(sa.px.size()));
    // The frame crosses tile borders (x = 3 and -3, z = -3 are in view).
    CHECK(visible_ground(f, {3.0f, 0.0f, 1.5f}));
    CHECK(visible_ground(f, {-3.0f, 0.0f, -3.0f}));
}

TEST_CASE("sun cookie: a black cookie skips the sun's direct term only; the ambient is unchanged") {
    Fixture3D f;
    const u32 black = solid_cookie(f, {0, 0, 0});
    const Shot none = draw_sun(f, SunCookie{}, 0.0f);
    const Shot dark = draw_sun(f, sun_cookie_key(black));
    REQUIRE(dark.stats.sun_cookie);
    CHECK(same(dark.px, none.px));  // the frame whose sun has no intensity: the same ambient, no direct sun
    // With the cascades on and a low sun too (the shadow term, which the ambient reads, is untouched).
    const vec3 low = math::normalize(vec3{-0.5f, -1.0f, -0.3f});
    CHECK(same(draw_sun(f, sun_cookie_key(black), 1.5f, low, true).px, draw_sun(f, SunCookie{}, 0.0f, low, true).px));
}

TEST_CASE("sun cookie: a cookie and the sun's shadow multiply: what the box shadows stays as dark") {
    Fixture3D f;
    const u32 key = half_cookie(f);
    const u32 white = solid_cookie(f, {1, 1, 1});
    const vec3 dir = math::normalize(vec3{-0.5f, -1.0f, -0.3f});
    const Shot unshadowed = draw_sun(f, SunCookie{}, 1.5f, dir, false);
    const Shot none = draw_sun(f, SunCookie{}, 0.0f, dir, true);
    const Shot plain = draw_sun(f, SunCookie{}, 1.5f, dir, true);
    const Shot shaped = draw_sun(f, sun_cookie_key(key, 1.5f), 1.5f, dir, true);
    REQUIRE(shaped.stats.sun_cookie);
    CHECK(same(draw_sun(f, sun_cookie_key(white, 1.5f), 1.5f, dir, true).px, plain.px));
    // Where the box's shadow darkens the ground the cookie never brightens it, and where the shadow takes the whole sun
    // (the plain frame is the sun-less one there) the pixel is the shadowed plain frame exactly, under any texel.
    u32 shadowed = 0, umbra = 0, changed = 0;
    for (usize i = 0; i < plain.px.size(); i += 4) {
        const int lu = (unshadowed.px[i] * 2 + unshadowed.px[i + 1] * 5 + unshadowed.px[i + 2]) / 8;
        const int lp = (plain.px[i] * 2 + plain.px[i + 1] * 5 + plain.px[i + 2]) / 8;
        const int ls = (shaped.px[i] * 2 + shaped.px[i + 1] * 5 + shaped.px[i + 2]) / 8;
        auto eq = [i](const Shot& a, const Shot& b) {
            return a.px[i] == b.px[i] && a.px[i + 1] == b.px[i + 1] && a.px[i + 2] == b.px[i + 2];
        };
        if (lu > lp + 25) {
            ++shadowed;
            CHECK(ls <= lp);
            if (eq(plain, none)) {
                ++umbra;
                CHECK(eq(shaped, plain));
            }
        } else if (!eq(shaped, plain)) {
            ++changed;
        }
    }
    MESSAGE(shadowed << " shadowed pixels (" << umbra << " in the umbra), " << changed << " lit pixels shaped");
    CHECK(umbra > 40);
    CHECK(changed > 1000);
}

TEST_CASE("sun cookie: a terrain gets the sun's cookie through the same lit body") {
    Fixture3D f;
    const u32 key = corner_cookie(f);
    TerrainShading shading;
    shading.layer_count = 1;
    shading.size = {12.0f, 12.0f};
    shading.splat_resolution = 2;
    auto terrain = [&](Scene3D& s) {
        MeshInstance t;
        t.mesh = 2;
        t.world = math::translation({0.0f, 0.02f, 0.0f});
        t.terrain = &shading;
        s.add(t);
    };
    const SunCookie c = sun_cookie_key(key);
    const Shot none = draw_sun(f, SunCookie{}, 0.0f, kSunDown, false, terrain);
    const Shot plain = draw_sun(f, SunCookie{}, 1.5f, kSunDown, false, terrain);
    const Shot shaped = draw_sun(f, c, 1.5f, kSunDown, false, terrain);
    REQUIRE(shaped.stats.sun_cookie);
    const PatternCount n = check_corner(f, shaped, plain, none, kSunDown, c, 0.02f);
    CHECK(n.bright > 20);
    CHECK(n.dark > 40);
}

TEST_CASE("sun cookie: the mips average a fine pattern by distance, a coarse one keeps its contrast") {
    Fixture3D f;
    const u32 fine = upload_cookie(f, 256, 256, [](u32 x, u32 y) { return ((x + y) & 1u) ? vec3{1, 1, 1} : vec3{0, 0, 0}; });
    const u32 coarse =
        upload_cookie(f, 256, 256, [](u32 x, u32 y) { return (((x / 32) + (y / 32)) & 1u) ? vec3{1, 1, 1} : vec3{0, 0, 0}; });
    const Shot none = draw_sun(f, SunCookie{}, 0.0f);
    const Shot plain = draw_sun(f, SunCookie{});
    const Shot a = draw_sun(f, sun_cookie_key(fine, 4.0f));
    const Shot b = draw_sun(f, sun_cookie_key(coarse, 4.0f));
    int fine_lo = 255, fine_hi = 0, coarse_lo = 255, coarse_hi = 0;
    u32 probes = 0, between = 0;
    for (f32 z = -5.0f; z <= 2.6f; z += 0.23f)
        for (f32 x = -3.5f; x <= 3.5f; x += 0.29f) {
            const vec3 p{x, 0.0f, z};
            if (!visible_ground(f, p)) continue;
            const int lf = luma_at(f, a.px, p), lc = luma_at(f, b.px, p);
            fine_lo = std::min(fine_lo, lf);
            fine_hi = std::max(fine_hi, lf);
            coarse_lo = std::min(coarse_lo, lc);
            coarse_hi = std::max(coarse_hi, lc);
            ++probes;
            between += (lf > luma_at(f, none.px, p) + 10 && lf < luma_at(f, plain.px, p) - 10) ? 1u : 0u;
        }
    MESSAGE("spread: fine " << fine_hi - fine_lo << ", coarse " << coarse_hi - coarse_lo << "; " << between << " of " << probes
                            << " fine probes between no sun and the plain sun");
    CHECK(coarse_hi - coarse_lo > 40);
    CHECK((fine_hi - fine_lo) * 3 < coarse_hi - coarse_lo);
    CHECK(between == probes);
    CHECK(probes > 30);
}

TEST_CASE("sun cookie: the Low tier keeps the pattern; the sun's layer counts against the layer cap first") {
    Fixture3D f;
    const u32 key = corner_cookie(f);
    CvarScope q("r.quality", "low");
    const QualitySettings low = quality_settings(QualityTier::Low);
    const Shot plain = draw_sun(f, SunCookie{});
    const Shot none = draw_sun(f, SunCookie{}, 0.0f);
    const SunCookie c = sun_cookie_key(key);
    const Shot shaped = draw_sun(f, c);
    REQUIRE(shaped.stats.sun_cookie);
    const PatternCount n = check_corner(f, shaped, plain, none, kSunDown, c);
    CHECK(n.bright > 20);
    CHECK(n.dark > 40);
    // As many distinct spot cookies as the tier has layers: the sun took layer 0, so the last spot draws plain.
    Array<u32> keys;
    for (u32 i = 0; i < low.light_cookie_layers; ++i) keys.push_back(solid_cookie(f, {0, 0, 0}));
    const Shot capped = draw_sun(f, c, 1.5f, kSunDown, false, [&](Scene3D& s) {
        for (u32 i = 0; i < low.light_cookie_layers; ++i) {
            PointLight l = test_spot({-3.0f + 1.5f * f32(i), 3.0f, 1.0f}, keys[i]);
            l.spot_outer = math::radians(10.0f);
            l.spot_inner = math::radians(8.0f);
            s.add_point_light(l);
        }
    });
    CHECK(capped.stats.sun_cookie);
    CHECK(capped.stats.cookie_layers == low.light_cookie_layers);
    CHECK(capped.stats.cookie_lights == low.light_cookie_layers - 1);
}

TEST_CASE("sun cookie: two views with different sun cookies in one command buffer each match the view drawn alone") {
    Fixture3D f;
    const u32 key = corner_cookie(f);
    const u32 key2 = half_cookie(f);
    constexpr u32 kSmallW = 96, kSmallH = 72;
    rhi::TextureDesc td;
    td.size = {kSmallW, kSmallH};
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.small";
    const rhi::TextureHandle small = f.device->create_texture(td);
    // The big view: the corner cookie on the sun. The small one: the half cookie on the sun and the corner on a spot.
    auto record = [&](rhi::CommandBuffer& cmd, rhi::TextureHandle target, rhi::Extent2D size, u32 slot, bool first) {
        const Camera3D cam = orbit_camera({0, 0.5f, 0}, 0.0f, math::radians(35.0f), 7.0f);
        Scene3D scene(f.renderer.meshes(), mem::heap());
        scene.begin(cam, size);
        const bool big_view = size.width == kW;
        day(scene, sun_cookie_key(big_view ? key : key2), 1.5f, kSunDown, false);
        MeshInstance ground;
        ground.mesh = 2;
        scene.add(ground);
        MeshInstance box;
        box.mesh = 1;
        scene.add(box);
        if (!big_view) scene.add_point_light(test_spot({2.2f, 3.2f, 0.6f}, key));
        scene.build();
        Forward3D::ViewOutput out;
        out.slot = slot;
        out.first = first;
        f.renderer.record_3d(cmd, scene, target, out);
        const Forward3DStats st = f.renderer.forward_3d().last_stats();
        CHECK(st.sun_cookie);
        CHECK(st.cookie_lights == (big_view ? 0u : 1u));
    };
    Array<u8> big_alone, small_alone, big, small_px;
    big_alone.resize(usize(kW) * kH * 4);
    big.resize(usize(kW) * kH * 4);
    small_alone.resize(usize(kSmallW) * kSmallH * 4);
    small_px.resize(usize(kSmallW) * kSmallH * 4);
    for (int k = 0; k < 2; ++k) {
        f.device->begin_frame(f.frame++);
        rhi::CommandBuffer cmd;
        if (k == 0)
            record(cmd, f.target, {kW, kH}, 1, true);
        else
            record(cmd, small, {kSmallW, kSmallH}, 2, true);
        f.device->submit(cmd);
        if (k == 0)
            REQUIRE(f.device->read_pixels(f.target, rhi::Rect2D{0, 0, kW, kH}, big_alone.span()).has_value());
        else
            REQUIRE(f.device->read_pixels(small, rhi::Rect2D{0, 0, kSmallW, kSmallH}, small_alone.span()).has_value());
        f.device->end_frame();
    }
    for (int frame = 0; frame < 3; ++frame) {
        f.device->begin_frame(f.frame++);
        rhi::CommandBuffer cmd;
        const bool big_first = frame != 1;
        if (big_first) {
            record(cmd, f.target, {kW, kH}, 3, true);
            record(cmd, small, {kSmallW, kSmallH}, 4, false);
        } else {
            record(cmd, small, {kSmallW, kSmallH}, 4, true);
            record(cmd, f.target, {kW, kH}, 3, false);
        }
        f.device->submit(cmd);
        REQUIRE(f.device->read_pixels(f.target, rhi::Rect2D{0, 0, kW, kH}, big.span()).has_value());
        REQUIRE(f.device->read_pixels(small, rhi::Rect2D{0, 0, kSmallW, kSmallH}, small_px.span()).has_value());
        f.device->end_frame();
        INFO("frame " << frame << (big_first ? ", big view first" : ", small view first"));
        CHECK(f.device->validation_error_count() == 0);
        CHECK(same(big, big_alone));
        CHECK(same(small_px, small_alone));
    }
    f.device->destroy_texture(small);
}

// ---- M-LIGHTCOOKIE 40f: the cookies in the volumetric fog's light injection (kVolInjectFS) --------------------------

namespace {

Volumetrics shaft_fog() {
    Volumetrics v;
    v.enabled = true;
    v.density = 0.12f;
    v.height_falloff = 0.0f;
    v.anisotropy = 0.2f;
    v.max_distance = 16.0f;
    v.tile = 4;
    v.slices = 32;
    return v;
}

// A spot straight down beside the box, a wide hard cone through the fog.
PointLight shaft_spot(u32 cookie) {
    PointLight l;
    l.position = {1.5f, 3.5f, 1.0f};
    l.direction = {0.0f, -1.0f, 0.0f};
    l.radius = 8.0f;
    l.intensity = 40.0f;
    l.spot_inner = math::radians(28.0f);
    l.spot_outer = math::radians(30.0f);
    l.cookie = cookie;
    return l;
}

}  // namespace

TEST_CASE("light cookie shafts: a cookied spot shapes its light in the fog; without the cookie the shafts are as before") {
    Fixture3D f;
    f.pitch_deg = 15.0f;
    const u32 grid = window_cookie(f);
    const u32 white = solid_cookie(f, {1, 1, 1});
    const u32 black = solid_cookie(f, {0, 0, 0});
    const u32 half = half_cookie(f);
    auto fog_with = [&](bool spot, u32 cookie) {
        return draw(f, [=](Scene3D& s) {
            s.set_volumetrics(shaft_fog());
            Sky sky;  // a dark sky behind the shafts (the fog is drawn over it; a cleared background has none)
            sky.zenith = sky.horizon = sky.ground = {0.01f, 0.01f, 0.02f};
            sky.sun_intensity = 0.0f;
            sky.specular_ambient = 0.0f;
            s.set_sky(sky);
            if (spot) s.add_point_light(shaft_spot(cookie));
        });
    };
    const Shot no_spot = fog_with(false, ~0u);
    const Shot plain = fog_with(true, ~0u);
    REQUIRE(plain.stats.volumetrics);
    const Shot shaped = fog_with(true, grid);
    REQUIRE(shaped.stats.cookie_lights == 1);
    CHECK_FALSE(same(shaped.px, plain.px));
    CHECK(same(fog_with(true, white).px, plain.px));  // a white cookie: the plain shafts bit for bit
    CHECK(same(fog_with(true, black).px, no_spot.px));  // a black one: no light in the fog either
    {
        CvarScope cv("r.light_cookies", "0");
        CHECK(same(fog_with(true, grid).px, plain.px));
    }
    // The cookie only takes light away; the window grid darkens the mullions' part of the glow.
    u32 brighter = 0, darker = 0;
    for (usize i = 0; i < shaped.px.size(); i += 4) {
        const int a = (shaped.px[i] * 2 + shaped.px[i + 1] * 5 + shaped.px[i + 2]) / 8;
        const int b = (plain.px[i] * 2 + plain.px[i + 1] * 5 + plain.px[i + 2]) / 8;
        brighter += a > b ? 1u : 0u;
        darker += a + 3 < b ? 1u : 0u;
    }
    MESSAGE("window grid in the fog: " << darker << " pixels darker than the plain shafts, " << brighter << " brighter");
    CHECK(brighter == 0);
    CHECK(darker > 100);
    // The half cookie (black where u < 0.5, toward +x for a spot straight down): in the air beside the axis the fog's
    // glow is gone on the dark side and kept on the bright one (the background behind is outside the cone).
    const Shot halved = fog_with(true, half);
    const PointLight l = shaft_spot(half);
    REQUIRE(cookie_uv(l, {l.position.x + 0.5f, 1.8f, l.position.z}).x < 0.5f);
    u32 probes = 0;
    for (f32 y = 1.4f; y <= 2.2f; y += 0.4f)
        for (f32 dx : {0.45f, 0.6f}) {
            const vec3 dark_p{l.position.x + dx, y, l.position.z}, lit_p{l.position.x - dx, y, l.position.z};
            const int glow_dark = luma_at(f, plain.px, dark_p) - luma_at(f, no_spot.px, dark_p);
            const int glow_lit = luma_at(f, plain.px, lit_p) - luma_at(f, no_spot.px, lit_p);
            MESSAGE("air probes at height " << y << ", " << dx << " m off the axis: plain glow " << glow_dark << " / " << glow_lit
                                            << ", halved " << luma_at(f, plain.px, dark_p) - luma_at(f, halved.px, dark_p) << " / "
                                            << luma_at(f, plain.px, lit_p) - luma_at(f, halved.px, lit_p) << " darker");
            if (glow_dark < 8 || glow_lit < 8) continue;
            ++probes;
            CHECK(luma_at(f, halved.px, dark_p) < luma_at(f, plain.px, dark_p) - glow_dark / 2);
            CHECK(luma_at(f, halved.px, lit_p) > luma_at(f, plain.px, lit_p) - glow_lit / 2);
        }
    CHECK(probes >= 2);
}

TEST_CASE("light cookie shafts: the sun's cookie shapes the sun's light in the fog") {
    Fixture3D f;
    f.pitch_deg = 15.0f;
    const u32 stripes = upload_cookie(f, 64, 64, [](u32 x, u32) { return (x / 16) % 2 ? vec3{1, 1, 1} : vec3{0, 0, 0}; });
    const u32 black = solid_cookie(f, {0, 0, 0});
    const u32 white = solid_cookie(f, {1, 1, 1});
    const vec3 dir = math::normalize(vec3{0.6f, -0.6f, 1.0f});  // low, in front of the camera (volumetric_shafts' sun)
    auto fog = [](Scene3D& s) {
        Volumetrics v = shaft_fog();
        v.density = 0.05f;
        s.set_volumetrics(v);
        Sky sky;  // a dark gradient without a sun disk: the sky does not read the sun's intensity, the fog over it does
        sky.zenith = sky.horizon = sky.ground = {0.02f, 0.02f, 0.03f};
        sky.sun_intensity = 0.0f;
        sky.specular_ambient = 0.0f;
        s.set_sky(sky);
    };
    const Shot plain = draw_sun(f, SunCookie{}, 1.0f, dir, true, fog);
    const Shot off = draw_sun(f, SunCookie{}, 0.0f, dir, true, fog);
    const Shot shaped = draw_sun(f, sun_cookie_key(stripes, 1.5f), 1.0f, dir, true, fog);
    REQUIRE(shaped.stats.sun_cookie);
    REQUIRE(shaped.stats.volumetrics);
    CHECK_FALSE(same(shaped.px, plain.px));
    CHECK(same(draw_sun(f, sun_cookie_key(white, 1.5f), 1.0f, dir, true, fog).px, plain.px));
    CHECK(same(draw_sun(f, sun_cookie_key(black, 1.5f), 1.0f, dir, true, fog).px, off.px));  // nor in the fog
    // In the sky part of the frame (fog only, no surface) the stripes darken some columns of the glow.
    u32 darker = 0;
    for (u32 y = 2; y < kH / 4; ++y)
        for (u32 x = 0; x < kW; ++x) {
            const usize i = (usize(y) * kW + x) * 4;
            const int a = (shaped.px[i] * 2 + shaped.px[i + 1] * 5 + shaped.px[i + 2]) / 8;
            const int b = (plain.px[i] * 2 + plain.px[i + 1] * 5 + plain.px[i + 2]) / 8;
            darker += a + 2 < b ? 1u : 0u;
        }
    MESSAGE(darker << " fog-only pixels darkened by the striped sun cookie");
    CHECK(darker > 50);
}
