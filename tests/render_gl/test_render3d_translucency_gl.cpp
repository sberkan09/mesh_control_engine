// M-TRANSLUCENT 41d / 41e against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: thin-surface
// translucency and wrap lighting in the lit mesh shader (MCE_GLSL_MESH_FS, the decode of ObjectU::o_skin.w and
// PassU::p_translucency). Off (no bits, -r.translucency 0, Forward3DDesc::translucency false) is the plain frame byte for
// byte; a double-sided leaf card between the camera and a low sun glows with its own colour, as much as its front would
// reflect of the same light (the KHR mix), and front-lit it is no brighter than the plain card; the forward-scatter lobe
// (High) peaks where the view looks into the sun, pinned against the CPU reference (translucency.h) so a flipped sign
// or axis fails; a second card between it and the sun shadows its back (the flipped-offset back shadow); metal does not
// transmit; snow cover damps it; the sun's cookie multiplies it; an unlit material ignores the bits; an instanced batch
// glows as the single draw; wrap moves a sphere's lit edge past the terminator by the expected angle, and the band under
// the sun's cascades is measured (the plan's open question). 41e: a closed paper box around a point light glows on every
// face, also with the light's shadow (its single lookup takes the flipped offset); a spot behind a card lights it inside
// the cone only and its cookie multiplies the glow; wrap under a point light; the back side's ambient on Medium and High
// (the hemisphere gradient and the SH9 two-sided evaluation, against a card whose normal is reversed) and Low equal to the
// plain frame. One 160 x 120 frame per render.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <functional>

#include "fixture3d.h"
#include "mce/asset/material_translucency.h"
#include "mce/convar.h"
#include "mce/render/quality.h"
#include "mce/render/translucency.h"

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

constexpr u32 kCard = 60, kCardReversed = 61, kSphere = 62;

constexpr vec3 kLeaf{0.25f, 0.6f, 0.12f};

// No ambient, no sky, no fog, no bloom or FXAA: a card's pixel is its own light.
void dark(Scene3D& s, vec3 sun_dir, f32 sun_intensity = 2.0f, bool shadows = true, f32 ambient = 0.0f) {
    DirectionalLight sun;
    sun.direction = sun_dir;
    sun.intensity = sun_intensity;
    sun.shadows = shadows;
    sun.ambient_sky = sun.ambient_ground = {ambient, ambient, ambient};
    s.set_light(sun);
    Sky sky;
    sky.enabled = false;
    sky.specular_ambient = 0.0f;
    s.set_sky(sky);
    Fog fog;
    fog.color = {0.0f, 0.0f, 0.0f};
    s.set_fog(fog);
    PostFx post;
    post.bloom = false;
    post.fxaa = false;
    s.set_post(post);
}

struct Px {
    int x, y;
};

Px pixel_of(const Fixture3D& f, vec3 p) {
    const math::vec4 c = f.view.view_proj * math::vec4(p, 1.0f);
    const f32 x = (c.x / c.w * 0.5f + 0.5f) * kW, y = (0.5f - c.y / c.w * 0.5f) * kH;
    REQUIRE(x >= 0);
    REQUIRE(y >= 0);
    REQUIRE(x < kW);
    REQUIRE(y < kH);
    return {int(x), int(y)};
}

Rgb rgb_at(const Fixture3D& f, const Array<u8>& px, vec3 p) {
    const Px q = pixel_of(f, p);
    const usize i = (usize(q.y) * kW + usize(q.x)) * 4;
    return {px[i], px[i + 1], px[i + 2]};
}

int luma_at(const Fixture3D& f, const Array<u8>& px, vec3 p) { return rgb_at(f, px, p).luma(); }

bool same_at(const Fixture3D& f, const Array<u8>& a, const Array<u8>& b, vec3 p) {
    const Rgb x = rgb_at(f, a, p), y = rgb_at(f, b, p);
    return x.r == y.r && x.g == y.g && x.b == y.b;
}

u32 changed_count(const Array<u8>& a, const Array<u8>& b) {
    u32 n = 0;
    for (usize i = 0; i < a.size(); i += 4) n += (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2]) ? 1u : 0u;
    return n;
}

struct Shot {
    Array<u8> px;
    Forward3DStats stats;
};

Shot draw(Fixture3D& f, const std::function<void(Scene3D&)>& setup) {
    f.render(0.0f, true, setup);
    CHECK(f.device->validation_error_count() == 0);
    return {f.pixels, f.renderer.forward_3d().last_stats()};
}

// ---- the leaf scene: a low camera, a 1.6 m card left of the fixture's box turned to face the camera, a low sun
// behind it (travelling toward the camera) or, mirrored through the card's plane, in front of it.
struct Leaf {
    vec3 center{-1.2f, 1.3f, 2.4f};
    vec3 normal;       // toward the camera
    vec3 back_sun;     // the way the light travels, from behind the card
    vec3 front_sun;    // back_sun mirrored through the card's plane: the same |n.l| on the front
    math::mat4 world;
};

Leaf leaf_scene(Fixture3D& f) {
    f.pitch_deg = 5.0f;
    f.look_at = {0.0f, 1.0f, 0.0f};
    upload_card(f, kCard, 0.8f);
    Leaf l;
    const vec3 cam = orbit_camera(f.look_at, 0.0f, math::radians(f.pitch_deg), 7.0f).position;
    l.normal = math::normalize(cam - l.center);
    l.back_sun = math::normalize(vec3{0.32f, -0.3f, 1.0f});  // toward the camera: the view looks into the sun
    const vec3 lb = -l.back_sun;
    const vec3 lf = lb - l.normal * (2.0f * math::dot(l.normal, lb));
    l.front_sun = -lf;
    l.world = card_world(l.center, l.normal);
    return l;
}

void add_card(Scene3D& s, const math::mat4& world, const MaterialOverride* m, u32 mesh = kCard) {
    MeshInstance c;
    c.mesh = mesh;
    c.world = world;
    c.material = m;
    s.add(c);
}

// A point on the card: (u, v) in -1..1 of its half extent along its local x and y.
vec3 on_card(const math::mat4& w, f32 half, f32 u, f32 v) {
    return vec3{w.c[3][0], w.c[3][1], w.c[3][2]} + vec3{w.c[0][0], w.c[0][1], w.c[0][2]} * (u * half) +
           vec3{w.c[1][0], w.c[1][1], w.c[1][2]} * (v * half);
}

}  // namespace

TEST_CASE("translucency: off (no bits, -r.translucency 0, Forward3DDesc::translucency false) draws the plain frame") {
    Fixture3D f;
    const Leaf leaf = leaf_scene(f);
    const MaterialOverride plain = thin_material(kLeaf, 0.0f, 0.0f);
    const MaterialOverride thin = thin_material(kLeaf, 0.8f, 0.3f);
    const auto scene = [&](const MaterialOverride* m) {
        return [&leaf, m](Scene3D& s) {
            dark(s, leaf.back_sun, 2.0f, true, 0.1f);
            add_card(s, leaf.world, m);
        };
    };
    const Shot a = draw(f, scene(&plain));
    CHECK(a.stats.translucent_draws == 0);
    const Shot b = draw(f, scene(&thin));
    CHECK(b.stats.translucent_draws == 1);
    CHECK_FALSE(same(a.px, b.px));
    MESSAGE(changed_count(a.px, b.px) << " pixels changed by translucency 0.8, wrap 0.3");
    {
        CvarScope cv("r.translucency", "0");
        const Shot off = draw(f, scene(&thin));
        CHECK(off.stats.translucent_draws == 0);
        CHECK(same(off.px, a.px));
    }
    const Shot again = draw(f, scene(&thin));  // read every frame
    CHECK(same(again.px, b.px));
    // The tiers bound the cost only: Low still draws it (the Lambert lobe, no scatter, no back ambient).
    {
        CvarScope q("r.quality", "low");
        const Shot low = draw(f, scene(&thin));
        CHECK(low.stats.translucent_draws == 1);
        CHECK_FALSE(same(low.px, a.px));
        CHECK_FALSE(same(low.px, b.px));
    }
}

TEST_CASE("translucency: Forward3DDesc::translucency false forces every draw's value to 0 for the renderer's life") {
    const MaterialOverride plain = thin_material(kLeaf, 0.0f, 0.0f);
    const MaterialOverride thin = thin_material(kLeaf, 0.8f, 0.3f);
    Forward3DDesc d;
    d.msaa = 4;
    d.translucency = false;
    Fixture3D g(4, true, true, 1.0f, &d);
    const Leaf leaf2 = leaf_scene(g);
    const auto scene2 = [&](const MaterialOverride* m) {
        return [&leaf2, m](Scene3D& s) {
            dark(s, leaf2.back_sun, 2.0f, true, 0.1f);
            add_card(s, leaf2.world, m);
        };
    };
    const Shot ga = draw(g, scene2(&plain));
    const Shot gb = draw(g, scene2(&thin));
    CHECK(gb.stats.translucent_draws == 0);
    CHECK(same(ga.px, gb.px));
}

TEST_CASE("translucency: a back-lit leaf card glows with its own colour, as much as its front reflects; front-lit it is no brighter") {
    Fixture3D f;
    const Leaf leaf = leaf_scene(f);
    const MaterialOverride plain = thin_material(kLeaf, 0.0f, 0.0f);
    const MaterialOverride full = thin_material(kLeaf, 1.0f, 0.0f);
    const MaterialOverride half = thin_material(kLeaf, 0.5f, 0.0f);
    const auto shot = [&](const MaterialOverride* m, vec3 sun) {
        return draw(f, [&leaf, m, sun](Scene3D& s) {
            dark(s, sun);
            add_card(s, leaf.world, m);
        });
    };
    const vec3 mid = leaf.center;
    const Shot dark_plain = shot(&plain, leaf.back_sun);
    const Shot glow = shot(&full, leaf.back_sun);
    const Rgb g = rgb_at(f, glow.px, mid), p = rgb_at(f, dark_plain.px, mid);
    MESSAGE("back-lit centre: plain " << p.r << "," << p.g << "," << p.b << "  translucent " << g.r << "," << g.g << "," << g.b);
    CHECK(p.luma() < 8);                 // back-lit and no ambient: black apart from nothing
    CHECK(g.luma() > p.luma() + 40);     // the glow
    CHECK(g.g > g.r + 20);               // its own (green) colour
    CHECK(g.g > g.b + 30);
    // The KHR mix: translucency 0.5 lets through, from behind, what the front reflects of the same |n.l| (Low: the
    // Lambert lobe only, which the front's Lambert matches but for its Fresnel and specular, a few percent here).
    {
        CvarScope q("r.quality", "low");
        const int back = luma_at(f, shot(&half, leaf.back_sun).px, mid);
        const int front = luma_at(f, shot(&half, leaf.front_sun).px, mid);
        MESSAGE("translucency 0.5, Low: back-lit " << back << ", front-lit " << front);
        CHECK(front > 30);
        CHECK(std::abs(back - front) <= std::max(3, front / 10));
    }
    // Front-lit, the translucent card reflects only its share: not brighter than the plain card (on every tier).
    for (const char* tier : {"high", "low"}) {
        CvarScope q("r.quality", tier);
        const int lp = luma_at(f, shot(&plain, leaf.front_sun).px, mid);
        const int lh = luma_at(f, shot(&half, leaf.front_sun).px, mid);
        const int lf = luma_at(f, shot(&full, leaf.front_sun).px, mid);
        MESSAGE(doctest::String(tier) << " front-lit: plain " << lp << ", translucency 0.5 " << lh << ", 1 " << lf);
        CHECK(lh <= lp);
        CHECK(lf <= lh);
        CHECK(lp > 40);
    }
}

TEST_CASE("translucency: the forward-scatter lobe (High) peaks where the view looks into the sun, as translucency_back says") {
    Fixture3D f;
    const Leaf leaf = leaf_scene(f);
    const MaterialOverride full = thin_material(kLeaf, 1.0f, 0.0f);
    const auto shot = [&](const char* tier) {
        CvarScope q("r.quality", tier);
        return draw(f, [&](Scene3D& s) {
            dark(s, leaf.back_sun);
            add_card(s, leaf.world, &full);
        });
    };
    const Shot high = shot("high"), low = shot("low");
    REQUIRE(high.stats.translucent_draws == 1);
    // The CPU reference of the lobe at four points of the card (v from the point to the camera; n the face toward it).
    const vec3 cam = f.view.position;
    const vec3 l = -leaf.back_sun;
    f32 lobe[4];
    int gain[4];
    const f32 uv[4][2] = {{-0.8f, 0.8f}, {0.8f, 0.8f}, {-0.8f, -0.8f}, {0.8f, -0.8f}};
    for (int i = 0; i < 4; ++i) {
        const vec3 p = on_card(leaf.world, 0.8f, uv[i][0], uv[i][1]);
        const vec3 v = math::normalize(cam - p);
        const f32 nl = math::dot(leaf.normal, l);
        const f32 vt = math::dot(v, -math::normalize(l + leaf.normal * kTranslucencyDistortion));
        lobe[i] = translucency_back(nl, vt, true) - translucency_back(nl, vt, false);
        gain[i] = luma_at(f, high.px, p) - luma_at(f, low.px, p);
        MESSAGE("corner " << i << ": lobe " << lobe[i] << ", High - Low " << gain[i]);
        CHECK(gain[i] >= 0);
    }
    // The strongest and the weakest corner by the reference are the strongest and the weakest on the GPU.
    const int hi = int(std::max_element(lobe, lobe + 4) - lobe), lo = int(std::min_element(lobe, lobe + 4) - lobe);
    REQUIRE(lobe[hi] > lobe[lo] * 1.3f);
    CHECK(gain[hi] > gain[lo] + 3);
    for (int i = 0; i < 4; ++i) CHECK(gain[i] <= gain[hi]);
}

TEST_CASE("translucency: a second card between the leaf and the sun shadows its back; the leaf does not shadow itself") {
    Fixture3D f;
    const Leaf leaf = leaf_scene(f);
    const MaterialOverride plain = thin_material(kLeaf, 0.0f, 0.0f);
    const MaterialOverride full = thin_material(kLeaf, 1.0f, 0.0f);
    const MaterialOverride grey = thin_material({0.4f, 0.4f, 0.4f}, 0.0f, 0.0f);
    const vec3 l = -leaf.back_sun;
    // The occluder: the same card 0.7 m toward the sun, so its shadow covers the leaf (the shift is along the light).
    const math::mat4 behind = card_world(leaf.center + l * 0.7f, leaf.normal);
    const auto shot = [&](const MaterialOverride* m, bool occluder) {
        return draw(f, [&](Scene3D& s) {
            dark(s, leaf.back_sun);
            add_card(s, leaf.world, m);
            if (occluder) add_card(s, behind, &grey);
        });
    };
    const vec3 mid = on_card(leaf.world, 0.8f, 0.0f, -0.3f);  // below the centre: the occluder hides behind the leaf there
    const int dark_plain = luma_at(f, shot(&plain, false).px, mid);
    const int open = luma_at(f, shot(&full, false).px, mid);
    const int shaded = luma_at(f, shot(&full, true).px, mid);
    const int shaded_plain = luma_at(f, shot(&plain, true).px, mid);
    MESSAGE("plain " << dark_plain << ", glowing " << open << ", behind a second card " << shaded << " (plain " << shaded_plain << ")");
    CHECK(open > dark_plain + 40);  // its own back is not shadowed by itself
    CHECK(shaded <= shaded_plain + 4);  // PCF's soft edge at most
}

TEST_CASE("translucency: a metallic card does not transmit; an unlit material ignores the bits") {
    Fixture3D f;
    const Leaf leaf = leaf_scene(f);
    const MaterialOverride metal = thin_material({0.9f, 0.8f, 0.5f}, 0.0f, 0.0f, 1.0f, 0.5f);
    const MaterialOverride metal_thin = thin_material({0.9f, 0.8f, 0.5f}, 1.0f, 0.0f, 1.0f, 0.5f);
    const auto shot = [&](const MaterialOverride* m) {
        return draw(f, [&](Scene3D& s) {
            dark(s, leaf.back_sun, 2.0f, true, 0.1f);
            add_card(s, leaf.world, m);
        });
    };
    const Shot a = shot(&metal), b = shot(&metal_thin);
    CHECK(b.stats.translucent_draws == 1);
    CHECK(same(a.px, b.px));
    const MaterialOverride unlit = thin_material(kLeaf, 0.0f, 0.0f, 0.0f, 0.9f, asset::MaterialFlags::DoubleSided | asset::MaterialFlags::Unlit);
    const MaterialOverride unlit_thin = thin_material(kLeaf, 1.0f, 1.0f, 0.0f, 0.9f, asset::MaterialFlags::DoubleSided | asset::MaterialFlags::Unlit);
    const Shot c = shot(&unlit), d = shot(&unlit_thin);
    CHECK(d.stats.translucent_draws == 0);
    CHECK(same(c.px, d.px));
}

TEST_CASE("translucency: metallic 0.5 lets through (1 - metallic) of its own (1 - metallic) diffuse") {
    // tcol = base * (1 - m) * t * (1 - m): metallic 0.5 with translucency 1 glows as a dielectric with translucency
    // 0.25, not 0.5 (which a tr without its (1 - metallic) would give). Back-lit, no ambient: the glow is tcol alone.
    Fixture3D f;
    const Leaf leaf = leaf_scene(f);
    const MaterialOverride half_metal = thin_material(kLeaf, 1.0f, 0.0f, 0.5f);
    const MaterialOverride quarter = thin_material(kLeaf, 0.25f, 0.0f);
    const MaterialOverride half = thin_material(kLeaf, 0.5f, 0.0f);
    const auto shot = [&](const MaterialOverride* m) {
        return draw(f, [&](Scene3D& s) {
            dark(s, leaf.back_sun);
            add_card(s, leaf.world, m);
        });
    };
    const Shot a = shot(&half_metal), b = shot(&quarter), c = shot(&half);
    for (f32 u : {-0.5f, 0.0f, 0.5f}) {
        const vec3 p = on_card(leaf.world, 0.8f, u, 0.0f);
        const int la = luma_at(f, a.px, p), lb = luma_at(f, b.px, p), lc = luma_at(f, c.px, p);
        MESSAGE("u " << u << ": metallic 0.5 translucency 1 " << la << ", dielectric 0.25 " << lb << ", 0.5 " << lc);
        CHECK(std::abs(la - lb) <= 2);
        CHECK(lc > lb + 8);
    }
}

TEST_CASE("translucency: snow cover damps it (a snowed-on leaf is a white, opaque leaf)") {
    Fixture3D f;
    upload_card(f, kCard, 0.8f);
    // A card lying flat 1.2 m up (its normal +y: snow lands on it), a point light 0.35 m under it, the camera above.
    const math::mat4 world = card_world({-1.2f, 1.2f, 1.2f}, {0, 1, 0}, {0, 0, -1});
    const MaterialOverride thin = thin_material(kLeaf, 1.0f, 0.0f);
    const auto shot = [&](f32 cover, bool light) {
        return draw(f, [&](Scene3D& s) {
            dark(s, {0.0f, -1.0f, 0.0f}, 0.0f, false, 0.1f);
            add_card(s, world, &thin);
            if (light) {
                PointLight p;
                p.position = {-1.2f, 0.85f, 1.2f};
                p.radius = 3.0f;
                p.intensity = 2.0f;
                s.add_point_light(p);
            }
            SnowCover snow;
            snow.cover = cover;
            snow.coverage = 1.0f;
            snow.noise_enabled = false;  // an even layer: the mask is cover * coverage
            snow.wind_bias = 0.0f;
            s.set_snow_cover(snow);
        });
    };
    const vec3 mid{-1.2f, 1.2f, 1.2f};
    const int bare_dark = luma_at(f, shot(0.0f, false).px, mid), bare_lit = luma_at(f, shot(0.0f, true).px, mid);
    const int half_dark = luma_at(f, shot(0.5f, false).px, mid), half_lit = luma_at(f, shot(0.5f, true).px, mid);
    const Shot full_dark = shot(1.0f, false), full_lit = shot(1.0f, true);
    MESSAGE("glow from below: bare " << bare_lit - bare_dark << ", half snow " << half_lit - half_dark << ", full snow "
                                     << luma_at(f, full_lit.px, mid) - luma_at(f, full_dark.px, mid));
    CHECK(bare_lit > bare_dark + 25);
    CHECK(half_lit - half_dark > 3);
    CHECK(half_lit - half_dark < bare_lit - bare_dark);
    for (f32 u : {-0.5f, 0.0f, 0.5f}) CHECK(same_at(f, full_dark.px, full_lit.px, on_card(world, 0.8f, u, 0.2f)));
}

TEST_CASE("translucency: a snowed-on leaf seen from below does not glow either (the mask of its upward face)") {
    // A card lying flat 4 m up (normal +y: snow lands on its top), a point light above it, the camera below looking up
    // at its underside: the pixel's face points down, where snow_mask is 0, so the damping takes the upward face's mask.
    Fixture3D f;
    f.look_at = {0.0f, 4.0f, 0.0f};
    f.pitch_deg = -20.0f;
    upload_card(f, kCard, 0.8f);
    const vec3 mid{0.0f, 4.0f, 0.0f};
    const math::mat4 world = card_world(mid, {0, 1, 0}, {0, 0, -1});
    const MaterialOverride thin = thin_material(kLeaf, 1.0f, 0.0f);
    const auto shot = [&](f32 cover, bool light) {
        return draw(f, [&](Scene3D& s) {
            dark(s, {0.0f, -1.0f, 0.0f}, 0.0f, false, 0.1f);
            add_card(s, world, &thin);
            if (light) {
                PointLight p;
                p.position = mid + vec3{0.0f, 0.35f, 0.0f};
                p.radius = 3.0f;
                p.intensity = 0.5f;
                s.add_point_light(p);
            }
            SnowCover snow;
            snow.cover = cover;
            snow.coverage = 1.0f;
            snow.noise_enabled = false;
            snow.wind_bias = 0.0f;
            s.set_snow_cover(snow);
        });
    };
    const int bare_dark = luma_at(f, shot(0.0f, false).px, mid), bare_lit = luma_at(f, shot(0.0f, true).px, mid);
    const int half_dark = luma_at(f, shot(0.5f, false).px, mid), half_lit = luma_at(f, shot(0.5f, true).px, mid);
    const Shot full_dark = shot(1.0f, false), full_lit = shot(1.0f, true);
    MESSAGE("glow seen from below: bare " << bare_lit - bare_dark << ", half snow " << half_lit - half_dark
                                          << ", full snow " << luma_at(f, full_lit.px, mid) - luma_at(f, full_dark.px, mid));
    CHECK(bare_lit > bare_dark + 25);
    CHECK(half_lit - half_dark > 3);
    CHECK(half_lit - half_dark < bare_lit - bare_dark);
    for (f32 u : {-0.5f, 0.0f, 0.5f}) CHECK(same_at(f, full_dark.px, full_lit.px, on_card(world, 0.8f, u, 0.0f)));
}

TEST_CASE("translucency: the sun's cookie multiplies the back-lit glow (white: the plain glow, black: none)") {
    Fixture3D f;
    const Leaf leaf = leaf_scene(f);
    const MaterialOverride full = thin_material(kLeaf, 1.0f, 0.2f);
    const auto solid = [&](u8 c) {
        Array<u8> t;
        for (int i = 0; i < 16; ++i)
            for (u8 b : {c, c, c, u8(255)}) t.push_back(b);
        const TextureMipUpload mip{4, 4, std::as_bytes(t.span())};
        TextureUpload up;
        up.mips = Span<const TextureMipUpload>(&mip, 1);
        const u32 key = f.renderer.textures().allocate_key();
        REQUIRE(f.renderer.textures().upload(key, 1, up).has_value());
        return key;
    };
    const u32 white = solid(255), black = solid(0);
    const auto shot = [&](u32 cookie, f32 intensity) {
        return draw(f, [&](Scene3D& s) {
            dark(s, leaf.back_sun, intensity, true, 0.08f);
            if (cookie != ~0u) {
                DirectionalLight sun = s.light();
                sun.cookie.texture = cookie;
                s.set_light(sun);
            }
            add_card(s, leaf.world, &full);
        });
    };
    const Shot plain = shot(~0u, 2.0f), lit_white = shot(white, 2.0f), lit_black = shot(black, 2.0f), sunless = shot(~0u, 0.0f);
    CHECK(lit_white.stats.sun_cookie);
    for (f32 u : {-0.6f, 0.0f, 0.6f})
        for (f32 v : {-0.6f, 0.0f, 0.6f}) {
            const vec3 p = on_card(leaf.world, 0.8f, u, v);
            CHECK(same_at(f, lit_white.px, plain.px, p));
            CHECK(same_at(f, lit_black.px, sunless.px, p));
        }
    CHECK(luma_at(f, plain.px, leaf.center) > luma_at(f, lit_black.px, leaf.center) + 40);
}

TEST_CASE("translucency: an instanced batch of leaf cards glows as the single draw") {
    Fixture3D f;
    const Leaf leaf = leaf_scene(f);
    const MaterialOverride plain = thin_material(kLeaf, 0.0f, 0.0f);
    const MaterialOverride full = thin_material(kLeaf, 1.0f, 0.0f);
    const math::mat4 xf[1] = {leaf.world};
    const auto shot = [&](const MaterialOverride* m, bool batch) {
        return draw(f, [&](Scene3D& s) {
            dark(s, leaf.back_sun);
            if (!batch) {
                add_card(s, leaf.world, m);
                return;
            }
            InstanceBatch b;
            b.id = 1;
            b.version = 1;
            b.mesh = kCard;
            b.transforms = Span<const math::mat4>(xf, 1);
            b.material = m;
            s.add_batch(b);
        });
    };
    const Shot single = shot(&full, false), batch = shot(&full, true), batch_plain = shot(&plain, true);
    CHECK(single.stats.translucent_draws == 1);
    CHECK(batch.stats.translucent_draws >= 1);
    CHECK(batch_plain.stats.translucent_draws == 0);
    for (f32 u : {-0.5f, 0.0f, 0.5f}) {
        const vec3 p = on_card(leaf.world, 0.8f, u, 0.0f);
        const int a = luma_at(f, single.px, p), b = luma_at(f, batch.px, p), c = luma_at(f, batch_plain.px, p);
        MESSAGE("u " << u << ": single " << a << ", batch " << b << ", plain batch " << c);
        CHECK(std::abs(a - b) <= 2);
        CHECK(b > c + 40);
    }
}

namespace {

// The wrap sphere: radius 1.2 at kWrapAt, white, roughness 0.8, the camera on it; the sun from the left (-x), so the
// terminator runs down the sphere's middle as the camera sees it.
const vec3 kWrapAt{-1.8f, 1.3f, 1.0f};
constexpr f32 kWrapR = 1.2f;
const vec3 kWrapSun = math::normalize(vec3{1.0f, -0.15f, -0.1f});

struct Band {
    u32 lit_plain = 0, lit_wrap = 0;  // sphere pixels brighter than black
};

// Pixels of the sphere's visible disk (a CPU ray cast) lit above `threshold` in each frame.
Band lit_pixels(const Fixture3D& f, const Array<u8>& plain, const Array<u8>& wrapped, int threshold) {
    Band b;
    const math::mat4 inv = math::inverse(f.view.view_proj);
    for (u32 y = 0; y < kH; ++y)
        for (u32 x = 0; x < kW; ++x) {
            const f32 nx = (f32(x) + 0.5f) / f32(kW) * 2.0f - 1.0f, ny = 1.0f - (f32(y) + 0.5f) / f32(kH) * 2.0f;
            const math::vec4 a = inv * math::vec4(nx, ny, -1.0f, 1.0f), c = inv * math::vec4(nx, ny, 1.0f, 1.0f);
            const vec3 o{a.x / a.w, a.y / a.w, a.z / a.w};
            const vec3 d = math::normalize(vec3{c.x / c.w, c.y / c.w, c.z / c.w} - o);
            const vec3 oc = o - kWrapAt;
            const f32 bq = math::dot(oc, d), cq = math::dot(oc, oc) - kWrapR * kWrapR * 0.97f;  // inside the rim
            if (bq * bq - cq < 0.0f) continue;
            const usize i = (usize(y) * kW + x) * 4;
            b.lit_plain += Rgb{plain[i], plain[i + 1], plain[i + 2]}.luma() > threshold ? 1u : 0u;
            b.lit_wrap += Rgb{wrapped[i], wrapped[i + 1], wrapped[i + 2]}.luma() > threshold ? 1u : 0u;
        }
    return b;
}

}  // namespace

TEST_CASE("translucency: wrap moves a sphere's lit edge past the terminator; the band under the sun's cascades (measured)") {
    Fixture3D f;
    f.look_at = kWrapAt;
    f.pitch_deg = 15.0f;
    upload_sphere(f, kSphere, kWrapAt, kWrapR);
    const MaterialOverride plain = thin_material({0.9f, 0.9f, 0.9f}, 0.0f, 0.0f, 0.0f, 0.8f, 0);
    const MaterialOverride wrap = thin_material({0.9f, 0.9f, 0.9f}, 0.0f, 0.5f, 0.0f, 0.8f, 0);
    const auto shot = [&](const MaterialOverride* m, bool shadows) {
        return draw(f, [&](Scene3D& s) {
            dark(s, kWrapSun, 2.0f, shadows);
            MeshInstance sp;
            sp.mesh = kSphere;
            sp.material = m;
            s.add(sp);
        });
    };
    const Shot plain_open = shot(&plain, false), wrap_open = shot(&wrap, false);
    CHECK(wrap_open.stats.translucent_draws == 1);
    // Along the visible great circle through the light: n = cos(phi) v0 + sin(phi) e1. Past n.l = -wrap the wrapped
    // frame stays black; inside the band it is lit while the plain one is black; on the lit side the wrap darkens
    // (energy: (n.l + w) / (1 + w)^2 at n.l = 1 is 0.67).
    const vec3 l = -kWrapSun;
    const vec3 v0 = math::normalize(f.view.position - kWrapAt);
    const vec3 e1 = math::normalize(l - v0 * math::dot(l, v0));
    const f32 w = asset::dequantize_unit(asset::quantize_unit(0.5f));
    u32 band_checked = 0;
    for (int k = -70; k <= 70; k += 5) {
        const f32 phi = math::radians(f32(k));
        const vec3 n = v0 * std::cos(phi) + e1 * std::sin(phi);
        const vec3 p = kWrapAt + n * (kWrapR * 0.995f);
        const f32 nl = math::dot(n, l);
        const int a = luma_at(f, plain_open.px, p), b = luma_at(f, wrap_open.px, p);
        INFO("phi " << k << " n.l " << nl << ": plain " << a << ", wrap " << b);
        if (nl < -w - 0.06f) {
            CHECK(a <= 1);
            CHECK(b <= 1);
        } else if (nl > -w + 0.12f && nl < -0.08f) {
            CHECK(a <= 1);
            CHECK(b > 4);
            ++band_checked;
        } else if (nl > 0.5f) {
            CHECK(b < a);
        }
    }
    CHECK(band_checked >= 3);
    // The measurement: sphere pixels lit with wrap 0.5 against wrap 0, without and with the sun's shadow (Risks).
    const Shot plain_sh = shot(&plain, true), wrap_sh = shot(&wrap, true);
    const Band open = lit_pixels(f, plain_open.px, wrap_open.px, 2), shadowed = lit_pixels(f, plain_sh.px, wrap_sh.px, 2);
    const f32 kept = f32(i32(shadowed.lit_wrap) - i32(shadowed.lit_plain)) / f32(std::max(1, i32(open.lit_wrap) - i32(open.lit_plain)));
    MESSAGE("wrap band, no shadows: " << open.lit_plain << " -> " << open.lit_wrap << " lit pixels; sun cascades: "
                                      << shadowed.lit_plain << " -> " << shadowed.lit_wrap << " (band kept " << kept * 100.0f << "%)");
    CHECK(open.lit_wrap > open.lit_plain + 20);
    CHECK(shadowed.lit_wrap >= shadowed.lit_plain);
    CHECK(shadowed.lit_wrap <= open.lit_wrap);
}

// ---- 41e: local lights and the ambient -------------------------------------------------------------------------------

TEST_CASE("translucency: a paper box around a point light glows on every face, also with the light's shadow") {
    Fixture3D f;
    const vec3 at{-1.6f, 0.0f, 1.4f};
    // The fixture's 1 m box (mesh 1, y 0..1) at 0.6 scale: a closed shade, culled back faces.
    const math::mat4 world = math::translation(at) * math::scaling({0.6f, 0.6f, 0.6f});
    const vec3 lamp = at + vec3{0.0f, 0.3f, 0.0f};
    const MaterialOverride paper = thin_material({0.9f, 0.8f, 0.6f}, 0.7f, 0.0f, 0.0f, 0.9f, 0);
    const MaterialOverride card = thin_material({0.9f, 0.8f, 0.6f}, 0.0f, 0.0f, 0.0f, 0.9f, 0);
    const auto shot = [&](const MaterialOverride* m, bool light, bool shadow) {
        return draw(f, [&](Scene3D& s) {
            dark(s, {0.0f, -1.0f, 0.0f}, 0.0f, false, 0.03f);
            MeshInstance box;
            box.mesh = 1;
            box.world = world;
            box.material = m;
            s.add(box);
            if (light) {
                PointLight p;
                p.position = lamp;
                p.radius = 4.0f;
                p.intensity = 0.12f;
                p.color = {1.0f, 0.8f, 0.5f};
                p.cast_shadows = shadow;
                s.add_point_light(p);
            }
            LocalShadowSettings ls;
            ls.tile = 256;  // six cube faces fit the 1024 atlas
            s.set_local_shadows(ls);
        });
    };
    const Shot dark_card = shot(&card, false, false);
    const Shot dark_paper = shot(&paper, false, false), lit_paper = shot(&paper, true, false), lit_card = shot(&card, true, false);
    const Shot shadowed = shot(&paper, true, true), shadowed_card = shot(&card, true, true);
    CHECK(shadowed.stats.local_shadow_tiles == 6);
    CHECK(lit_paper.stats.translucent_draws == 1);
    const vec3 faces[3] = {lamp + vec3{0.0f, 0.3f, 0.0f}, lamp + vec3{0.0f, 0.0f, 0.3f}, lamp + vec3{0.3f, 0.0f, 0.0f}};
    const char* names[3] = {"top", "front", "right"};
    for (int i = 0; i < 3; ++i) {
        const vec3 p = faces[i] * 0.995f + lamp * 0.005f;
        const int dc = luma_at(f, dark_card.px, p);
        const int d = luma_at(f, dark_paper.px, p), a = luma_at(f, lit_paper.px, p), c = luma_at(f, lit_card.px, p);
        const int s = luma_at(f, shadowed.px, p), sc = luma_at(f, shadowed_card.px, p);
        MESSAGE(doctest::String(names[i]) << ": unlit " << d << ", lantern " << a << ", opaque shade " << c << ", lantern with the light's shadow "
                         << s << " (opaque " << sc << ")");
        CHECK(a > c + 25);                   // the lamp is behind every outer face
        CHECK(std::abs(c - dc) <= 1);        // the opaque shade's outside gets nothing from the lamp inside
        CHECK(a < 245);                      // unsaturated: the shadow check below means something
        CHECK(s - d >= (a - d) * 6 / 10);    // the back lookup (flipped offset) does not black it out
    }
}

TEST_CASE("translucency: a spot behind a card lights it inside the cone only; its cookie multiplies the glow") {
    Fixture3D f;
    const Leaf leaf = leaf_scene(f);
    const MaterialOverride full = thin_material(kLeaf, 1.0f, 0.0f);
    const auto solid = [&](u8 c) {
        Array<u8> t;
        for (int i = 0; i < 16; ++i)
            for (u8 b : {c, c, c, u8(255)}) t.push_back(b);
        const TextureMipUpload mip{4, 4, std::as_bytes(t.span())};
        TextureUpload up;
        up.mips = Span<const TextureMipUpload>(&mip, 1);
        const u32 key = f.renderer.textures().allocate_key();
        REQUIRE(f.renderer.textures().upload(key, 1, up).has_value());
        return key;
    };
    const u32 white = solid(255), black = solid(0);
    // A spot 1 m behind the card's left part, pointing through it toward the camera, a 12 degree cone.
    const vec3 target = on_card(leaf.world, 0.8f, -0.5f, 0.2f);
    const PointLight spot = [&] {
        PointLight p;
        p.position = target - leaf.normal * 1.0f;
        p.direction = leaf.normal;
        p.radius = 4.0f;
        p.intensity = 3.0f;
        p.spot_inner = math::radians(10.0f);
        p.spot_outer = math::radians(12.0f);
        return p;
    }();
    const auto shot = [&](bool light, u32 cookie) {
        return draw(f, [&](Scene3D& s) {
            dark(s, {0.0f, -1.0f, 0.0f}, 0.0f, false, 0.05f);
            add_card(s, leaf.world, &full);
            if (light) {
                PointLight p = spot;
                p.cookie = cookie;
                s.add_point_light(p);
            }
        });
    };
    const Shot none = shot(false, ~0u), lit = shot(true, ~0u), lit_white = shot(true, white), lit_black = shot(true, black);
    CHECK(lit_white.stats.cookie_lights == 1);
    const int glow = luma_at(f, lit.px, target) - luma_at(f, none.px, target);
    MESSAGE("spot glow at the cone's centre " << glow);
    CHECK(glow > 30);
    for (vec3 p : {on_card(leaf.world, 0.8f, 0.6f, 0.0f), on_card(leaf.world, 0.8f, 0.5f, -0.6f), on_card(leaf.world, 0.8f, -0.5f, -0.8f)})
        CHECK(same_at(f, lit.px, none.px, p));  // outside the cone: untouched
    for (f32 du : {-0.1f, 0.0f, 0.1f}) {
        const vec3 p = on_card(leaf.world, 0.8f, -0.5f + du, 0.2f);
        CHECK(same_at(f, lit_white.px, lit.px, p));
        CHECK(same_at(f, lit_black.px, none.px, p));
    }
}

TEST_CASE("translucency: a grazing front-lit lamp seen from the other side adds no forward-scatter lobe") {
    // n.l = cos 80 = 0.17 on the front, the view at -80 degrees on the same side: translucency_back's lobe alone would be
    // about 0.27 here (v looks into the light), but no light comes through, so the back term is 0, as the sun's shb.
    // translucency 1 reflects no diffuse (diffuse_f 0), so the card draws as a black card of the same f0 and roughness.
    CvarScope q("r.quality", "high");
    Fixture3D f;
    const Leaf leaf = leaf_scene(f);
    constexpr u32 kBigCard = 63;
    upload_card(f, kBigCard, 1.5f);
    const vec3 c = leaf.center;
    const vec3 cam = orbit_camera(f.look_at, 0.0f, math::radians(f.pitch_deg), 7.0f).position;  // as leaf_scene
    const vec3 v = math::normalize(cam - c);
    const vec3 u = math::normalize(math::cross(vec3{0, 1, 0}, v));
    const f32 c80 = std::cos(math::radians(80.0f)), s80 = std::sin(math::radians(80.0f));
    const vec3 n = v * c80 + u * s80;  // so v = n cos 80 - t sin 80
    const vec3 t = v * -s80 + u * c80;
    const vec3 l = math::normalize(n * c80 + t * s80);  // l = n cos 80 + t sin 80
    const f32 nl = math::dot(n, l), vt = math::dot(v, -math::normalize(l + n * kTranslucencyDistortion));
    const f32 lobe = translucency_back(nl, vt, true) - translucency_back(nl, vt, false);
    MESSAGE("n.l " << nl << ", n.v " << math::dot(n, v) << ", the lobe the CPU formula alone would give " << lobe);
    REQUIRE(lobe > 0.2f);
    const math::mat4 world = card_world(c, n);
    const MaterialOverride plain = thin_material(kLeaf, 0.0f, 0.0f);
    const MaterialOverride black = thin_material({0.0f, 0.0f, 0.0f}, 0.0f, 0.0f);
    const MaterialOverride full = thin_material(kLeaf, 1.0f, 0.0f);
    const auto shot = [&](const MaterialOverride* m) {
        return draw(f, [&](Scene3D& s) {
            dark(s, {0.0f, -1.0f, 0.0f}, 0.0f, false);
            add_card(s, world, m, kBigCard);
            PointLight p;
            p.position = c + l * 1.2f;
            p.radius = 6.0f;
            p.intensity = 1.5f;
            s.add_point_light(p);
        });
    };
    const Shot a = shot(&plain), b = shot(&black), d = shot(&full);
    CHECK(d.stats.translucent_draws == 1);
    const vec3 side = math::normalize(math::cross(n, t));
    for (f32 dv : {-0.3f, 0.0f, 0.3f}) {
        const vec3 p = c + side * dv;
        const int lp = luma_at(f, a.px, p), lb = luma_at(f, b.px, p), lf = luma_at(f, d.px, p);
        MESSAGE("dv " << dv << ": plain " << lp << ", black " << lb << ", translucency 1 " << lf);
        CHECK(lp > lb + 15);  // the lamp lights the front
        CHECK(std::abs(lf - lb) <= 2);
    }
}

TEST_CASE("translucency: wrap under a point light without shadows") {
    Fixture3D f;
    f.look_at = kWrapAt;
    f.pitch_deg = 15.0f;
    upload_sphere(f, kSphere, kWrapAt, kWrapR);
    const MaterialOverride plain = thin_material({0.9f, 0.9f, 0.9f}, 0.0f, 0.0f, 0.0f, 0.8f, 0);
    const MaterialOverride wrap = thin_material({0.9f, 0.9f, 0.9f}, 0.0f, 0.5f, 0.0f, 0.8f, 0);
    const vec3 lamp = kWrapAt + vec3{-4.0f, 0.4f, 0.0f};
    const auto shot = [&](const MaterialOverride* m) {
        return draw(f, [&](Scene3D& s) {
            dark(s, {0.0f, -1.0f, 0.0f}, 0.0f, false);
            MeshInstance sp;
            sp.mesh = kSphere;
            sp.material = m;
            s.add(sp);
            PointLight p;
            p.position = lamp;
            p.radius = 12.0f;
            p.intensity = 30.0f;
            s.add_point_light(p);
        });
    };
    const Shot a = shot(&plain), b = shot(&wrap);
    const vec3 v0 = math::normalize(f.view.position - kWrapAt);
    u32 band = 0;
    for (int k = -70; k <= 70; k += 5) {
        const f32 phi = math::radians(f32(k));
        const vec3 e1 = math::normalize((lamp - kWrapAt) - v0 * math::dot(lamp - kWrapAt, v0));
        const vec3 n = v0 * std::cos(phi) + e1 * std::sin(phi);
        const vec3 p = kWrapAt + n * (kWrapR * 0.995f);
        const f32 nl = math::dot(n, math::normalize(lamp - p));
        const int la = luma_at(f, a.px, p), lb = luma_at(f, b.px, p);
        INFO("phi " << k << " n.l " << nl << ": plain " << la << ", wrap " << lb);
        if (nl > -0.4f && nl < -0.08f) {
            CHECK(la <= 1);
            CHECK(lb > 3);
            ++band;
        } else if (nl < -0.56f) {
            CHECK(lb <= 1);
        }
    }
    CHECK(band >= 3);
}

TEST_CASE("translucency: the back side's ambient (Medium and High) against Low, which is the plain frame") {
    // A card lying flat (normal +y), seen from above, under a blue sky ambient and a red ground ambient; no sun light (its
    // direction straight up, so a card whose normal points down faces it: the sky's flat ambient is not darkened there).
    Fixture3D f;
    upload_card(f, kCard, 0.8f);
    upload_card(f, kCardReversed, 0.8f, true);
    const math::mat4 world = card_world({-1.2f, 1.0f, 1.4f}, {0, 1, 0}, {0, 0, -1});
    const MaterialOverride plain = thin_material({0.8f, 0.8f, 0.8f}, 0.0f, 0.0f);
    const MaterialOverride full = thin_material({0.8f, 0.8f, 0.8f}, 1.0f, 0.0f);
    const MaterialOverride half = thin_material({0.8f, 0.8f, 0.8f}, 0.5f, 0.0f);
    const auto shot = [&](const MaterialOverride* m, u32 mesh, bool ibl) {
        return draw(f, [&](Scene3D& s) {
            dark(s, {0.0f, 1.0f, 0.0f}, 0.0f, false);
            DirectionalLight sun = s.light();
            sun.ambient_sky = {0.05f, 0.1f, 0.6f};
            sun.ambient_ground = {0.6f, 0.08f, 0.04f};
            s.set_light(sun);
            if (ibl) {
                Sky sky;
                sky.zenith = {0.05f, 0.1f, 0.6f};
                sky.horizon = {0.2f, 0.2f, 0.25f};
                sky.ground = {0.6f, 0.08f, 0.04f};
                sky.specular_ambient = 0.0f;
                sky.ibl = true;
                s.set_sky(sky);
            }
            add_card(s, world, m, mesh);
        });
    };
    const vec3 mid{-1.2f, 1.0f, 1.4f};
    for (bool ibl : {false, true}) {
        INFO((ibl ? "SH9 sky irradiance" : "hemisphere gradient"));
        const Shot p = shot(&plain, kCard, ibl);
        {
            CvarScope q("r.quality", "low");  // Low: the full diffuse takes the front's ambient, as today
            CHECK(same(shot(&full, kCard, ibl).px, p.px));
        }
        for (const char* tier : {"high", "medium"}) {
            CvarScope q("r.quality", tier);
            const Shot t = shot(&full, kCard, ibl), h = shot(&half, kCard, ibl);
            // translucency 1: the front reflects nothing, the back lets the ground's (red) light through: the card
            // whose normal points down, lit by the same ambient, is the reference (sh_irradiance(-n) or the gradient at -n).
            const Shot r = shot(&plain, kCardReversed, ibl);
            const Rgb ct = rgb_at(f, t.px, mid), cr = rgb_at(f, r.px, mid), cp = rgb_at(f, p.px, mid), ch = rgb_at(f, h.px, mid);
            MESSAGE(doctest::String(tier) << ": plain " << cp.r << "," << cp.g << "," << cp.b << "  t 0.5 " << ch.r << "," << ch.g << "," << ch.b
                         << "  t 1 " << ct.r << "," << ct.g << "," << ct.b << "  reversed " << cr.r << "," << cr.g << "," << cr.b);
            CHECK(ct.r > ct.b + 30);  // the ground's light, not the sky's
            CHECK(cp.b > cp.r + 30);
            CHECK(std::abs(ct.r - cr.r) <= 1);
            CHECK(std::abs(ct.g - cr.g) <= 1);
            CHECK(std::abs(ct.b - cr.b) <= 1);
            CHECK(ch.r > cp.r);
            CHECK(ch.b < cp.b);
        }
    }
}
