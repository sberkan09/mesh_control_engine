// M-CLOUDSHADOW 51c against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: cloud shadows, one
// fullscreen pass that rebuilds each pixel's position from the half-resolution opaque prepass, moves it along the sun to
// the sky's cloud plane, reads the sky's noise there and multiplies the pixel by cloud_shadow_factor, after the exposure
// histogram and before the sun shafts (Forward3D::record_cloud_shadow, kCloudShadowFS).
// The off paths (the setting off, the Low tier, r.cloud_shadows 0, coverage 0, the cloud layer off, a sun under the fade
// band or below the horizon, a zero strength, a render-to-texture view) draw the frame without the pass bit for bit; with
// the pass only ground pixels change and only darker (sky pixels stay as they are, but for the half-resolution prepass's
// one-pixel silhouette); more coverage shades more ground; the patches move with the scene clock along the wind and a
// 180 degree wind turn moves them the other way; a floating-origin rebase is bit for bit; the shade fades as the sun
// sinks toward the horizon; Medium (2 octaves) and High (3) both draw; with TAA the next frame holds no shade; a second
// window view of another size draws; and the shader alone agrees with the CPU twin (cloud_shadow.h) on a grid of points.
// One 160 x 120 frame per render.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>

#include "fixture3d.h"
#include "mce/convar.h"
#include "mce/render/cloud_shadow.h"
#include "mce/render/quality.h"
#include "shaders3d.h"  // render's 3D shaders: the pass runs alone

using namespace mce;
using namespace mce::render;
using namespace mce::render_test;

namespace {

bool same(const Array<u8>& a, const Array<u8>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

int luma8(const Array<u8>& px, usize p) {
    return (int(px[p * 4]) * 2 + int(px[p * 4 + 1]) * 5 + int(px[p * 4 + 2])) / 8;
}

bool same_px(const Array<u8>& a, const Array<u8>& b, usize p) {
    return a[p * 4] == b[p * 4] && a[p * 4 + 1] == b[p * 4 + 1] && a[p * 4 + 2] == b[p * 4 + 2];
}

struct Shot {
    Array<u8> px;
    Forward3DStats stats;
};

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

struct ShadowFixture {
    Fixture3D f;
    ShadowFixture() { cloud_shadow_camera(f); }
    Shot draw(const Clouds& clouds, const CloudShadow* shadow, f32 time = 0.0f, vec3 toward = kCloudShadowToward,
              bool ground = true, const std::function<void(Scene3D&)>& more = {}) {
        f.render(0.0f, false, [&](Scene3D& s) {
            cloud_shadow_scene(s, clouds, shadow, time, toward, ground);
            if (more) more(s);
        });
        CHECK(f.device->validation_error_count() == 0);
        return {f.pixels, f.renderer.forward_3d().last_stats()};
    }
};

// What the pass did to a frame: per pixel the luma it took away (off - on, 0..255; negative = it brightened).
struct Shade {
    i64 sum = 0;       // summed luma taken away
    usize shaded = 0;  // pixels darker by at least 2 luma
    int brightest = 0;  // the largest brightening (should be 0: the pass only darkens)
};

Shade shade(const Array<u8>& on, const Array<u8>& off) {
    Shade s;
    for (usize p = 0; p < usize(kW) * kH; ++p) {
        const int d = luma8(off, p) - luma8(on, p);
        s.sum += d;
        if (d >= 2) ++s.shaded;
        s.brightest = std::max(s.brightest, -d);
    }
    return s;
}

// The sky mask at full resolution: a pixel is sky where the frame with the ground equals the frame without it.
Array<u8> sky_mask(const Array<u8>& with_ground, const Array<u8>& sky_only) {
    Array<u8> m;
    m.resize(usize(kW) * kH);
    for (usize p = 0; p < m.size(); ++p) m[p] = same_px(with_ground, sky_only, p) ? 1 : 0;
    return m;
}

// The darkening map (off - on luma) of rows y0..y1 and the horizontal shift (pixels, -r..r) that best maps `a` onto `b`
// (b(x) ~ a(x - shift)): positive = the pattern moved right on screen.
int best_shift(const Array<u8>& a_on, const Array<u8>& a_off, const Array<u8>& b_on, const Array<u8>& b_off, int y0,
               int y1, int r) {
    int best = 0;
    f64 best_err = 1e300;
    for (int s = -r; s <= r; ++s) {
        f64 err = 0.0;
        usize n = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = r; x < int(kW) - r; ++x) {
                const usize pb = usize(y) * kW + usize(x), pa = usize(y) * kW + usize(x - s);
                const f64 da = luma8(a_off, pa) - luma8(a_on, pa), db = luma8(b_off, pb) - luma8(b_on, pb);
                err += (da - db) * (da - db);
                ++n;
            }
        err /= f64(n);
        if (err < best_err) {
            best_err = err;
            best = s;
        }
    }
    return best;
}

// The first row (top first) whose middle pixel is ground in the sky mask.
int first_ground_row(const Array<u8>& sky) {
    for (u32 y = 0; y < kH; ++y)
        if (!sky[usize(y) * kW + kW / 2]) return int(y);
    return int(kH);
}

}  // namespace

TEST_CASE("cloud shadow: the off paths draw the frame without the pass bit for bit") {
    ShadowFixture h;
    const Clouds clouds = cloud_shadow_clouds(0.55f);
    const CloudShadow on = cloud_shadow_setting();
    const Shot plain = h.draw(clouds, nullptr);
    CHECK_FALSE(plain.stats.cloud_shadow);
    CHECK(plain.stats.cloud_shadow_strength == 0.0f);
    CloudShadow off = on;
    off.enabled = false;
    CloudShadow weak = on;
    weak.strength = 0.0f;
    for (const CloudShadow* cs : {&off, &weak}) {
        const Shot s = h.draw(clouds, cs);
        CHECK_FALSE(s.stats.cloud_shadow);
        CHECK(same(s.px, plain.px));
    }
    for (const auto& [name, value] : {std::pair{"r.quality", "low"}, std::pair{"r.cloud_shadows", "0"}}) {
        CvarScope scope(name, value);
        const Shot base = h.draw(clouds, nullptr);
        const Shot s = h.draw(clouds, &on);
        CHECK_FALSE(s.stats.cloud_shadow);
        CHECK(same(s.px, base.px));
    }
    // No coverage, or the layer switched off: nothing to cast a shadow.
    Clouds clear = clouds;
    clear.coverage = 0.0f;
    Clouds disabled = clouds;
    disabled.enabled = false;
    for (const Clouds* c : {&clear, &disabled}) {
        const Shot base = h.draw(*c, nullptr);
        const Shot s = h.draw(*c, &on);
        CHECK_FALSE(s.stats.cloud_shadow);
        CHECK(same(s.px, base.px));
    }
    // A sun under the fade band (sin of the elevation below 0.02), below the horizon, a NaN and a zero direction: the
    // frame with the same sun and no pass.
    const f32 nan = std::nanf("");
    for (const vec3 toward : {vec3{0.0f, 0.015f, -1.0f}, vec3{0.3f, -0.5f, -1.0f}, vec3{nan, 1.0f, 0.0f},
                              vec3{0.0f, 0.0f, 0.0f}}) {
        const Shot base = h.draw(clouds, nullptr, 0.0f, toward);
        const Shot s = h.draw(clouds, &on, 0.0f, toward);
        CHECK_FALSE(s.stats.cloud_shadow);
        CHECK(same(s.px, base.px));
    }
    // The sun without light (intensity 0): its share is 0, nothing to take away.
    const Shot dark_base = h.draw(clouds, nullptr, 0.0f, kCloudShadowToward, true, [](Scene3D& s) {
        DirectionalLight l = s.light();
        l.intensity = 0.0f;
        s.set_light(l);
    });
    const Shot dark = h.draw(clouds, &on, 0.0f, kCloudShadowToward, true, [](Scene3D& s) {
        DirectionalLight l = s.light();
        l.intensity = 0.0f;
        s.set_light(l);
    });
    CHECK_FALSE(dark.stats.cloud_shadow);
    CHECK(same(dark.px, dark_base.px));
    // On: the pass runs, with the setting's strength.
    const Shot s = h.draw(clouds, &on);
    CHECK(s.stats.cloud_shadow);
    CHECK(s.stats.cloud_shadow_strength == doctest::Approx(0.8f));
    CHECK_FALSE(same(s.px, plain.px));
    // The same frame twice: the same bytes.
    CHECK(same(h.draw(clouds, &on).px, s.px));
}

TEST_CASE("cloud shadow: only ground pixels change, only darker; the sky stays as it is") {
    ShadowFixture h;
    const Clouds clouds = cloud_shadow_clouds(0.55f);
    const CloudShadow on = cloud_shadow_setting();
    const Shot off = h.draw(clouds, nullptr);
    const Shot lit = h.draw(clouds, &on);
    const Shot sky_only = h.draw(clouds, nullptr, 0.0f, kCloudShadowToward, false);
    const Shot sky_only_on = h.draw(clouds, &on, 0.0f, kCloudShadowToward, false);
    REQUIRE(lit.stats.cloud_shadow);
    // The sky alone: the pass runs and changes nothing.
    CHECK(sky_only_on.stats.cloud_shadow);
    CHECK(same(sky_only_on.px, sky_only.px));
    const Array<u8> sky = sky_mask(off.px, sky_only.px);
    usize sky_px = 0, ground_px = 0, sky_changed = 0, sky_changed_far = 0, ground_changed = 0;
    for (u32 y = 0; y < kH; ++y)
        for (u32 x = 0; x < kW; ++x) {
            const usize p = usize(y) * kW + x;
            const bool changed = !same_px(off.px, lit.px, p);
            if (!sky[p]) {
                ++ground_px;
                ground_changed += changed ? 1 : 0;
                continue;
            }
            ++sky_px;
            if (!changed) continue;
            ++sky_changed;
            // A changed sky pixel must sit on the silhouette: a ground pixel within one pixel (the half-resolution
            // prepass's 2 x 2 block).
            bool near_ground = false;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = int(x) + dx, yy = int(y) + dy;
                    if (xx >= 0 && yy >= 0 && xx < int(kW) && yy < int(kH) && !sky[usize(yy) * kW + usize(xx)])
                        near_ground = true;
                }
            sky_changed_far += near_ground ? 0 : 1;
        }
    MESSAGE("sky pixels " << sky_px << " (changed on the silhouette " << sky_changed << "), ground pixels " << ground_px
                          << " (changed " << ground_changed << ")");
    CHECK(sky_px > usize(kW) * 20);
    CHECK(ground_px > usize(kW) * 40);
    CHECK(sky_changed_far == 0);
    CHECK(sky_changed <= usize(kW));  // at most one row's worth along the horizon
    CHECK(ground_changed > ground_px / 5);
    const Shade sh = shade(lit.px, off.px);
    CHECK(sh.brightest == 0);
    CHECK(sh.shaded > 0);
}

TEST_CASE("cloud shadow: more coverage shades more of the ground") {
    ShadowFixture h;
    const CloudShadow on = cloud_shadow_setting();
    i64 last_sum = -1;
    usize last_shaded = 0;
    for (const f32 coverage : {0.25f, 0.5f, 0.8f}) {
        const Clouds c = cloud_shadow_clouds(coverage);
        const Shade s = shade(h.draw(c, &on).px, h.draw(c, nullptr).px);
        MESSAGE("coverage " << coverage << ": shaded pixels " << s.shaded << ", luma taken " << s.sum);
        CHECK(s.brightest == 0);
        CHECK(s.sum > last_sum);
        CHECK(s.shaded >= last_shaded);
        last_sum = s.sum;
        last_shaded = s.shaded;
    }
    CHECK(last_shaded > usize(kW) * 20);
}

TEST_CASE("cloud shadow: the exp² fog keeps the fogged distance from darkening") {
    ShadowFixture h;
    const Clouds clouds = cloud_shadow_clouds(0.55f);
    const CloudShadow on = cloud_shadow_setting();
    const auto fog = [](Scene3D& s) {
        Fog f = s.fog();
        f.density = 0.004f;  // a surface 300 m away keeps exp(-1.44), about a quarter, of its own colour
        s.set_fog(f);
    };
    const Shade clear = shade(h.draw(clouds, &on).px, h.draw(clouds, nullptr).px);
    const Shot fog_on = h.draw(clouds, &on, 0.0f, kCloudShadowToward, true, fog);
    const Shade fogged = shade(fog_on.px, h.draw(clouds, nullptr, 0.0f, kCloudShadowToward, true, fog).px);
    MESSAGE("luma taken without fog " << clear.sum << ", with fog " << fogged.sum);
    CHECK(fog_on.stats.cloud_shadow);
    CHECK(fogged.brightest == 0);
    CHECK(fogged.sum > 0);
    CHECK(f64(fogged.sum) < 0.6 * f64(clear.sum));
}

TEST_CASE("cloud shadow: the patches move with the clock along the wind; a 180 degree turn moves them back") {
    ShadowFixture h;
    const CloudShadow on = cloud_shadow_setting();
    Clouds east = cloud_shadow_clouds(0.5f);  // wind +x: the camera's right
    Clouds west = east;
    west.wind_dir = {-1.0f, 0.0f};
    const Shot e0_on = h.draw(east, &on, 0.0f), e0_off = h.draw(east, nullptr, 0.0f);
    const Shot e1_on = h.draw(east, &on, 1.0f), e1_off = h.draw(east, nullptr, 1.0f);
    const Shot w1_on = h.draw(west, &on, 1.0f), w1_off = h.draw(west, nullptr, 1.0f);
    CHECK_FALSE(same(e0_on.px, e1_on.px));
    const Array<u8> sky = sky_mask(e0_off.px, h.draw(east, nullptr, 0.0f, kCloudShadowToward, false).px);
    const int y0 = std::max(first_ground_row(sky), int(kH) - 40);  // the nearest ground: the largest motion in pixels
    const int east_shift = best_shift(e0_on.px, e0_off.px, e1_on.px, e1_off.px, y0, int(kH), 24);
    const int west_shift = best_shift(e0_on.px, e0_off.px, w1_on.px, w1_off.px, y0, int(kH), 24);
    MESSAGE("1 s of wind +x moves the shade " << east_shift << " px, wind -x " << west_shift << " px");
    CHECK(east_shift > 2);
    CHECK(west_shift < -2);
    CHECK(std::abs(east_shift + west_shift) <= 2);
    // Speed 0 holds the shade still at any time.
    Clouds still = east;
    still.speed = 0.0f;
    CHECK(same(h.draw(still, &on, 0.0f).px, h.draw(still, &on, 1234.5f).px));
}

TEST_CASE("cloud shadow: a floating-origin rebase is bit for bit") {
    ShadowFixture h;
    const Clouds clouds = cloud_shadow_clouds(0.55f);
    const CloudShadow on = cloud_shadow_setting();
    const Shot home = h.draw(clouds, &on, 20.0f);
    REQUIRE(home.stats.cloud_shadow);
    const auto rebase = [](Scene3D& s) { s.set_origin_shift(vec3{1000.0f, 0.0f, 1000.0f}); };
    const Shot rebased = h.draw(clouds, &on, 20.0f, kCloudShadowToward, true, rebase);
    CHECK(rebased.stats.cloud_shadow);
    CHECK(same(rebased.px, home.px));
    const auto far = [](Scene3D& s) { s.set_origin_shift(vec3{-250000.0f, 40.0f, 125000.0f}); };
    CHECK(same(h.draw(clouds, &on, 20.0f, kCloudShadowToward, true, far).px, home.px));
}

TEST_CASE("cloud shadow: the shade fades to nothing as the sun sinks toward the horizon") {
    ShadowFixture h;
    const Clouds clouds = cloud_shadow_clouds(0.55f);
    const CloudShadow on = cloud_shadow_setting();
    // The sun ahead of the camera at a falling elevation (sin = y of the unit direction).
    i64 last = -1;
    for (const f32 y : {0.019f, 0.03f, 0.08f, 0.15f, 0.25f}) {
        const f32 hz = std::sqrt(1.0f - y * y);
        const vec3 toward{0.6f * hz, y, -0.8f * hz};
        const Shot base = h.draw(clouds, nullptr, 0.0f, toward);
        const Shot s = h.draw(clouds, &on, 0.0f, toward);
        const Shade sh = shade(s.px, base.px);
        MESSAGE("sun elevation sin " << y << ": luma taken " << sh.sum << " over " << sh.shaded << " pixels");
        if (y < kCloudShadowMinSunY) {
            CHECK_FALSE(s.stats.cloud_shadow);
            CHECK(same(s.px, base.px));
        }
        CHECK(sh.brightest == 0);
        // The shade per unit of the sunlit frame's luma grows with the elevation's fade (smoothstep 0.02..0.25); the
        // frame itself darkens toward the horizon too, so compare the share taken.
        i64 total = 0;
        for (usize p = 0; p < usize(kW) * kH; ++p) total += luma8(base.px, p);
        const i64 per_mille = total > 0 ? sh.sum * 1000 / total : 0;
        CHECK(per_mille >= last);
        last = per_mille;
    }
    CHECK(last > 0);
}

TEST_CASE("cloud shadow: Medium (2 octaves) and High (3) both draw, differently; Low draws nothing") {
    ShadowFixture h;
    const Clouds clouds = cloud_shadow_clouds(0.55f);
    const CloudShadow on = cloud_shadow_setting();
    Array<u8> frames[2];
    for (u32 k = 0; k < 2; ++k) {
        CvarScope scope("r.quality", k == 0 ? "medium" : "high");
        const Shot s = h.draw(clouds, &on);
        CHECK(s.stats.cloud_shadow);
        frames[k] = s.px;
        CHECK(shade(s.px, h.draw(clouds, nullptr).px).shaded > usize(kW) * 10);
    }
    CHECK_FALSE(same(frames[0], frames[1]));
}

TEST_CASE("cloud shadow: with TAA the shade never enters the history") {
    const Clouds clouds = cloud_shadow_clouds(0.55f);
    const CloudShadow on = cloud_shadow_setting();
    const auto taa = [](Scene3D& s) {
        PostFx post = s.post();
        post.taa = true;
        s.set_post(post);
    };
    auto frames = [&](bool shade_first) {
        ShadowFixture h;
        for (int i = 0; i < 2; ++i) {
            const Shot s = h.draw(clouds, shade_first ? &on : nullptr, 0.0f, kCloudShadowToward, true, taa);
            CHECK(s.stats.taa);
            CHECK(s.stats.cloud_shadow == shade_first);
        }
        const Shot last = h.draw(clouds, nullptr, 0.0f, kCloudShadowToward, true, taa);
        CHECK(last.stats.taa_history);
        CHECK_FALSE(last.stats.cloud_shadow);
        return last.px;
    };
    CHECK(same(frames(true), frames(false)));
}

TEST_CASE("cloud shadow: a render-to-texture view records none; a second window view of another size draws") {
    ShadowFixture h;
    const Clouds clouds = cloud_shadow_clouds(0.55f);
    const CloudShadow on = cloud_shadow_setting();
    REQUIRE(h.draw(clouds, &on).stats.cloud_shadow);
    // One frame of two views: a render-texture view first (slot 1, the frame's first: it draws no shade), then the window
    // view (slot 0, not first) at a smaller size than the frame before it (its targets are recreated).
    auto frame = [&](const CloudShadow* cs, Forward3DStats stats[2]) {
        h.f.device->begin_frame(h.f.frame++);
        rhi::CommandBuffer cmd;
        const rhi::Extent2D sizes[2] = {{kW, kH}, {128, 96}};
        for (u32 v = 0; v < 2; ++v) {
            Scene3D scene(h.f.renderer.meshes(), mem::heap());
            Camera3D cam = orbit_camera(h.f.look_at, 0.0f, math::radians(h.f.pitch_deg), 7.0f);
            cam.far_z = h.f.far_z;
            scene.begin(cam, sizes[v]);
            cloud_shadow_scene(scene, clouds, cs, 0.0f);
            scene.build();
            Forward3D::ViewOutput out;
            out.slot = v == 0 ? 1u : 0u;
            out.first = v == 0;
            out.linear_output = v == 0;
            if (v == 1) out.rect = rhi::Rect2D{0, 0, sizes[v].width, sizes[v].height};
            h.f.renderer.record_3d(cmd, scene, h.f.target, out);
            stats[v] = h.f.renderer.forward_3d().last_stats();
        }
        h.f.device->submit(cmd);
        REQUIRE(h.f.device->read_pixels(h.f.target, rhi::Rect2D{0, 0, kW, kH}, h.f.pixels.span()).has_value());
        h.f.device->end_frame();
        CHECK(h.f.device->validation_error_count() == 0);
        return h.f.pixels;
    };
    Forward3DStats with_stats[2], without_stats[2];
    const Array<u8> with = frame(&on, with_stats);
    const Array<u8> without = frame(nullptr, without_stats);
    CHECK_FALSE(with_stats[0].cloud_shadow);
    CHECK(with_stats[1].cloud_shadow);
    for (const Forward3DStats& s : without_stats) CHECK_FALSE(s.cloud_shadow);
    // Only the window view's rectangle (128 x 96 from a corner; rows 24..95 lie inside it whichever way the rows run):
    // the render-texture view's pixels around it are not part of the comparison.
    int brightest = 0;
    usize shaded = 0;
    for (u32 y = 24; y < 96; ++y)
        for (u32 x = 0; x < 128; ++x) {
            const usize p = usize(y) * kW + x;
            const int d = luma8(without, p) - luma8(with, p);
            brightest = std::max(brightest, -d);
            shaded += d >= 2 ? 1 : 0;
        }
    MESSAGE("the window view after the render-texture view: " << shaded << " shaded pixels");
    CHECK(brightest == 0);
    CHECK(shaded > 500);
}

// ---- the shader alone against the CPU twin ---------------------------------------------------------------------------

namespace {

// f32 to IEEE half for the depths the case uploads (small integers and 0: exact).
u16 to_half(f32 v) {
    u32 b;
    std::memcpy(&b, &v, 4);
    const u32 sign = (b >> 16) & 0x8000u;
    const i32 e = i32((b >> 23) & 0xffu) - 127 + 15;
    u32 m = b & 0x7fffffu;
    if (e <= 0) return u16(sign);
    if (e >= 31) return u16(sign | 0x7c00u);
    u32 h = sign | (u32(e) << 10) | (m >> 13);
    m &= 0x1fffu;
    if (m > 0x1000u || (m == 0x1000u && (h & 1u))) ++h;
    return u16(h);
}

rhi::TextureHandle row_texture(Fixture3D& f, u32 w, const std::function<math::vec4(u32)>& texel) {
    Array<u16> data;
    data.resize(usize(w) * 4);
    for (u32 x = 0; x < w; ++x) {
        const math::vec4 c = texel(x);
        data[x * 4] = to_half(c.x);
        data[x * 4 + 1] = to_half(c.y);
        data[x * 4 + 2] = to_half(c.z);
        data[x * 4 + 3] = to_half(c.w);
    }
    rhi::TextureDesc d;
    d.size = {w, 1};
    d.format = rhi::TextureFormat::RGBA16F;
    d.usage = rhi::TextureUsage::Sampled;
    d.debug_name = "test.cloud_shadow.input";
    return f.device->create_texture(d, Span<const byte>(reinterpret_cast<const byte*>(data.data()), data.size() * 2));
}

// Reads a float texture's red channel bit for bit: a pass writes the float's bits as four bytes.
constexpr const char* kEncodeFS = R"(#version 450 core
layout(binding = 6) uniform sampler2D u_src;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    o_color = unpackUnorm4x8(floatBitsToUint(texelFetch(u_src, ivec2(gl_FragCoord.xy), 0).r));
}
)";

rhi::PipelineHandle fullscreen_pipe(Fixture3D& f, const char* fs, const char* name) {
    rhi::GraphicsPipelineDesc pd;
    pd.vertex_shader = {shaders3d::kFullscreenVS, "fullscreen.vert"};
    pd.fragment_shader = {fs, name};
    pd.state.depth = {};
    pd.debug_name = name;
    auto pipe = f.device->create_pipeline(pd);
    REQUIRE(pipe.has_value());
    return pipe.value();
}

// CloudShadowU as the shader declares it (forward3d.cpp's mirror is private to the renderer).
struct CloudShadowBlock {
    f32 proj[4], right[4], up[4], back[4], sun[4], shade[4], drift[4];
};
static_assert(sizeof(CloudShadowBlock) == shaders3d::kCloudShadowUBytes, "CloudShadowBlock mirrors CloudShadowU");

// kCloudShadowFS alone over a w x 1 prepass row (depth per texel) and a white source into a w x 1 RGBA32F target:
// returns the factor per texel. One row, so v_uv.y is 0.5 whatever the backend's row order.
Array<f32> run_pass(Fixture3D& f, const Array<f32>& depth, const CloudShadowBlock& u) {
    const u32 w = u32(depth.size());
    const rhi::TextureHandle gbuf = row_texture(f, w, [&](u32 x) { return math::vec4(0.0f, 0.0f, 1.0f, depth[x]); });
    const rhi::TextureHandle src = row_texture(f, w, [](u32) { return math::vec4(1.0f, 1.0f, 1.0f, 1.0f); });
    const rhi::PipelineHandle pipe = fullscreen_pipe(f, shaders3d::kCloudShadowFS, "cloud_shadow.frag");
    const rhi::PipelineHandle enc = fullscreen_pipe(f, kEncodeFS, "test.cloud_shadow.encode");
    rhi::TextureDesc td;
    td.size = {w, 1};
    td.format = rhi::TextureFormat::RGBA32F;
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.cloud_shadow.out";
    const rhi::TextureHandle rt = f.device->create_texture(td);
    td.format = rhi::TextureFormat::RGBA8_UNorm;
    td.debug_name = "test.cloud_shadow.encode";
    const rhi::TextureHandle bytes = f.device->create_texture(td);
    rhi::SamplerDesc pd;
    pd.min_filter = pd.mag_filter = rhi::Filter::Nearest;
    const rhi::SamplerHandle point = f.device->create_sampler(pd);
    f.device->begin_frame(f.frame++);
    rhi::CommandBuffer cmd;
    auto pass = [&](rhi::TextureHandle dst, const char* name) {
        rhi::PassAttachment o;
        o.texture = dst;
        rhi::RenderPassDesc p;
        p.colors = Span<const rhi::PassAttachment>(&o, 1);
        p.debug_name = name;
        cmd.begin_pass(p);
    };
    pass(rt, "test.cloud_shadow");
    cmd.bind_pipeline(pipe);
    cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(u));
    cmd.bind_texture(rhi::bank::kAlbedo, src, point);
    cmd.bind_texture(rhi::bank::kNormal, gbuf, point);
    cmd.draw_non_indexed(3);
    cmd.end_pass();
    pass(bytes, "test.cloud_shadow.encode");
    cmd.bind_pipeline(enc);
    cmd.bind_texture(rhi::bank::kAlbedo, rt, point);
    cmd.draw_non_indexed(3);
    cmd.end_pass();
    f.device->submit(cmd);
    Array<u8> px;
    px.resize(usize(w) * 4);
    REQUIRE(f.device->read_pixels(bytes, rhi::Rect2D{0, 0, w, 1}, px.span()).has_value());
    f.device->end_frame();
    CHECK(f.device->validation_error_count() == 0);
    Array<f32> out;
    out.resize(w);
    for (u32 i = 0; i < w; ++i) {
        const u32 b = u32(px[i * 4]) | (u32(px[i * 4 + 1]) << 8) | (u32(px[i * 4 + 2]) << 16) | (u32(px[i * 4 + 3]) << 24);
        std::memcpy(&out[i], &b, 4);
    }
    for (rhi::TextureHandle t : {gbuf, src, rt, bytes}) f.device->destroy_texture(t);
    f.device->destroy_pipeline(pipe);
    f.device->destroy_pipeline(enc);
    f.device->destroy_sampler(point);
    return out;
}

}  // namespace

TEST_CASE("cloud shadow: the shader agrees with the CPU helpers on a grid of points") {
    Fixture3D f;
    constexpr u32 kN = 64;
    Array<f32> depth;
    depth.resize(kN);
    for (u32 x = 0; x < kN; ++x) depth[x] = x % 16 == 0 ? 0.0f : f32(2 + 7 * x);  // 0 = sky; 9..443 m, exact in half
    struct Case {
        f32 yaw, pitch;  // the camera's yaw and pitch (radians; pitch > 0 looks down)
        vec3 toward;     // toward the sun (normalized as the renderer does)
        f32 coverage, density, scale, height;
        math::vec2 drift;
        u32 octaves;
        f32 softness, strength, share;
        f32 fog = 0.0f;  // the exp² fog density (the shade scales by the surface's share exp(-(fog * distance)²))
    };
    const Case cases[] = {
        {0.0f, 0.3f, {0.35f, 0.8f, -0.45f}, 0.55f, 0.7f, 60.0f, 150.0f, {0.0f, 0.0f}, 3, 0.3f, 0.8f, 0.82f},
        {1.1f, 0.6f, {-0.5f, 0.4f, 0.2f}, 0.8f, 0.3f, 120.0f, 400.0f, {12.25f, 63.5f}, 2, 0.0f, 1.0f, 1.0f},
        {-2.4f, 0.1f, {0.1f, 0.15f, -0.9f}, 0.35f, 1.0f, 50.0f, 100.0f, {31.0f, 7.75f}, 1, 1.0f, 0.6f, 0.5f},
        {3.0f, -0.5f, {0.0f, 1.0f, 0.0f}, 1.0f, 0.0f, 900.0f, 2000.0f, {0.5f, 0.25f}, 3, 0.5f, 0.9f, 0.7f},  // looking up
        {0.7f, 0.9f, {0.6f, 0.03f, 0.8f}, 0.6f, 0.7f, 75.0f, 120.0f, {50.0f, 1.0f}, 3, 0.2f, 1.0f, 1.0f},     // low sun
        {0.2f, 0.4f, {0.3f, 0.7f, -0.4f}, 0.7f, 0.7f, 60.0f, 150.0f, {3.5f, 9.0f}, 3, 0.3f, 1.0f, 0.9f, 0.004f},  // fog
    };
    const f32 fov = math::radians(45.0f), aspect = f32(kN);  // a w x 1 target: aspect w
    usize compared = 0, hits = 0, shaded = 0;
    f32 worst = 0.0f;
    for (const Case& c : cases) {
        // The camera basis of a yaw / pitch orbit camera (looking along -z at yaw 0, pitch 0).
        const f32 cy = std::cos(c.yaw), sy = std::sin(c.yaw), cp = std::cos(c.pitch), sp = std::sin(c.pitch);
        const vec3 fwd{-sy * cp, -sp, -cy * cp};
        const vec3 right = math::normalize(math::cross(fwd, vec3{0.0f, 1.0f, 0.0f}));
        const vec3 up = math::cross(right, fwd);
        const vec3 back = -fwd;
        const f32 p11 = 1.0f / std::tan(fov * 0.5f), p00 = p11 / aspect * 40.0f;  // a wide row: about 70 degrees across
        const f32 p20 = 0.0125f, p21 = -0.02f;                                     // an off-centre (jittered) projection
        const f32 len = std::sqrt(c.toward.x * c.toward.x + c.toward.y * c.toward.y + c.toward.z * c.toward.z);
        const vec3 sun{c.toward.x / len, c.toward.y / len, c.toward.z / len};
        CloudShadowBlock u{};
        const f32 blk[7][4] = {{p00, p11, p20, p21},
                               {right.x, right.y, right.z, 1.0f / c.scale},
                               {up.x, up.y, up.z, c.height},
                               {back.x, back.y, back.z, c.coverage},
                               {sun.x, sun.y, sun.z, c.density},
                               {c.strength, c.share, c.softness, f32(c.octaves)},
                               {c.drift.x, c.drift.y, c.fog, 0.0f}};
        std::memcpy(&u, blk, sizeof u);
        const Array<f32> gpu = run_pass(f, depth, u);
        CloudDensityParams dp;
        dp.coverage = c.coverage;
        dp.density = c.density;
        dp.scale_m = c.scale;
        dp.drift_cells = c.drift;
        dp.octaves = c.octaves;
        dp.softness = c.softness;
        for (u32 x = 0; x < kN; ++x) {
            f32 want = 1.0f;
            const f32 z = depth[x];
            if (z > 0.0f) {
                const f32 ndc_x = (f32(x) + 0.5f) / f32(kN) * 2.0f - 1.0f;
                const vec3 v{(ndc_x + p20) * z / p00, (0.0f + p21) * z / p11, -z};
                const vec3 p{right.x * v.x + up.x * v.y + back.x * v.z, right.y * v.x + up.y * v.y + back.y * v.z,
                             right.z * v.x + up.z * v.y + back.z * v.z};
                math::vec2 xz;
                if (cloud_shadow_point(p, sun, vec3{0.0f, 0.0f, 0.0f}, c.height, xz)) {
                    want = cloud_shadow_factor(cloud_density(xz, dp), c.strength, c.share, sun.y);
                    if (c.fog > 0.0f) {  // only the surface's share of the pixel: 1 - (1 - factor) * exp(-(fog * |v|)²)
                        const f32 fd = c.fog * std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
                        want = 1.0f - (1.0f - want) * std::exp(-fd * fd);
                    }
                    ++hits;
                    shaded += want < 0.99f ? 1 : 0;
                }
            }
            const f32 err = std::fabs(gpu[x] - want);
            worst = std::max(worst, err);
            if (z == 0.0f) CHECK(gpu[x] == 1.0f);  // sky: exactly untouched
            CHECK_MESSAGE(err <= 2e-4f, "case yaw " << c.yaw << " texel " << x << ": gpu " << gpu[x] << " cpu " << want);
            ++compared;
        }
    }
    MESSAGE("shader vs CPU twin: " << compared << " points (" << hits << " under the plane, " << shaded
                                    << " shaded), worst |diff| " << worst);
    CHECK(hits > compared / 2);
    CHECK(shaded > compared / 8);
}

// The pass's GPU cost per tier, opt in (MCE_CLOUD_SHADOW_COST=1; not part of the regular run): the bloom bucket of the GPU
// timestamps (kTsPostFx..kTsBloom: the cloud shadow, the shafts, the flare, the underwater look, the lens rain and the haze;
// only the cloud shadow is on here) at 1280 x 720, the mean of 16 frames after 4 warm-up frames, with and without the
// pass. Prints, asserts nothing about the numbers.
TEST_CASE("cloud shadow: GPU cost per tier at 1280 x 720 (opt in)") {
    if (!std::getenv("MCE_CLOUD_SHADOW_COST")) return;
    ShadowFixture h;
    if (!h.f.device->caps().timestamp_queries) return;
    const rhi::Extent2D size{1280, 720};
    rhi::TextureDesc td;
    td.size = size;
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.cloud_shadow.cost";
    const rhi::TextureHandle target = h.f.device->create_texture(td);
    const Clouds clouds = cloud_shadow_clouds(0.55f);
    const CloudShadow on = cloud_shadow_setting();
    auto mean_ms = [&](const CloudShadow* cs) {
        f64 sum = 0.0;
        u32 n = 0;
        for (u32 i = 0; i < 20; ++i) {
            h.f.device->begin_frame(h.f.frame++);
            rhi::CommandBuffer cmd;
            Scene3D scene(h.f.renderer.meshes(), mem::heap());
            Camera3D cam = orbit_camera(h.f.look_at, 0.0f, math::radians(h.f.pitch_deg), 7.0f);
            cam.far_z = h.f.far_z;
            scene.begin(cam, size);
            cloud_shadow_scene(scene, clouds, cs, 0.0f);
            scene.build();
            Forward3D::ViewOutput out;
            out.first = true;
            out.rect = rhi::Rect2D{0, 0, size.width, size.height};
            h.f.renderer.record_3d(cmd, scene, target, out);
            h.f.device->submit(cmd);
            h.f.device->end_frame();
            const Forward3DGpuTimes t = h.f.renderer.forward_3d().gpu_times(h.f.device->last_gpu_timestamps());
            if (i >= 4) {
                sum += t.bloom;
                ++n;
            }
        }
        return sum / f64(n);
    };
    for (u32 k = 0; k < 2; ++k) {
        CvarScope scope("r.quality", k == 0 ? "medium" : "high");
        const f64 without = mean_ms(nullptr), with = mean_ms(&on);
        MESSAGE("cloud shadow cost, " << (k == 0 ? 2 : 3) << " octaves (Medium 2, High 3), 1280 x 720: bloom bucket "
                                      << without << " ms without, " << with << " ms with, pass ~" << with - without << " ms");
    }
    h.f.device->destroy_texture(target);
    CHECK(h.f.device->validation_error_count() == 0);
}
