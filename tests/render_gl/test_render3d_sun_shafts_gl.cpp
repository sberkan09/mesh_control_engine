// M-SUNSHAFTS 45c against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: sun light shafts, a
// half-resolution radial accumulation from the HDR frame and the opaque prepass into shafts_buf_ and a full-resolution
// composite into the post-fx ping-pong target, after the exposure histogram and before the flare
// (Forward3D::record_sun_shafts, kShaftsFS / kShaftsCompFS).
// The off paths (the setting off, the Low tier, r.sun_shafts 0, a sun below the horizon, a sun behind the camera, a
// render-to-texture view) draw the frame without the pass bit for bit and record nothing; with the sun in front the
// pass adds light around the sun and only there (no pixel darkens, pixels far from the sun are unchanged); a column
// over half of the disk leaves its side dark and the open side lit; a bright wall over the whole sky gives no rays (the
// sky gate: the HDR mask alone does not radiate); a cloud deck over the sun dims the rays through the HDR mask; 16
// (Medium) and 32 (High) samples agree in brightness; the same frame twice gives the same bytes; with TAA the next
// frame holds no rays; the volumetric fog backs the strength off to 0.4; sweeps of the sun across the border, through
// the horizon and around behind the camera do not pop; the measured sky and sun disk radiance sit on either side of the
// default threshold; the shaders run alone: the hash port is sun_shafts_hash bit for bit, the soft clamp never exceeds
// the cap and matches the C++ helper, 16 and 32 samples give the same mean, an off-screen sun does not smear the
// border, and geometry stops the rays. One 160 x 120 frame per render.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <utility>

#include "fixture3d.h"
#include "mce/convar.h"
#include "mce/render/quality.h"
#include "mce/render/sun_shafts.h"
#include "shaders3d.h"  // render's 3D shaders: the passes run alone, the hash port and the fullscreen vertex stage

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

struct Shot {
    Array<u8> px;
    Forward3DStats stats;
    View3D view;
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

// Where the sun goes: a uv of the frame (lens_flare_toward) or an explicit direction.
struct SunAt {
    f32 u = kShaftsSunU, v = kShaftsSunV;
    bool explicit_dir = false;
    vec3 dir{0.0f, 1.0f, 0.0f};
};

struct ShaftsFixture {
    Fixture3D f;
    ShaftsFixture() { sun_shafts_camera(f); }
    // One frame with `shafts` (null: none), the sun at `at`; `column` adds the column over the left half of the disk.
    Shot draw(const SunShafts* shafts, SunAt at = {}, bool column = false,
              const std::function<void(Scene3D&)>& more = {}, bool post_default = false) {
        f.render(0.0f, false, [&](Scene3D& s) {
            const vec3 toward = at.explicit_dir ? math::normalize(at.dir) : lens_flare_toward(s.view(), at.u, at.v);
            sun_shafts_scene(s, toward, shafts, post_default);
            if (column) sun_shafts_column(s, toward);
            if (more) more(s);
        });
        CHECK(f.device->validation_error_count() == 0);
        return {f.pixels, f.renderer.forward_3d().last_stats(), f.view};
    }
};

// The sun's pixel (x right, y down, read_pixels order) and the disk radius in pixels for the shafts camera.
struct SunPx {
    f32 x = 0.0f, y = 0.0f, r = 0.0f;
};

SunPx sun_px(SunAt at = {}) {
    // The default Sky's disk: cos 0.9994; the orbit camera's 45-degree vertical field of view over 120 rows.
    const f32 radius_rad = std::acos(0.9994f);
    const f32 r = std::tan(radius_rad) / std::tan(math::radians(22.5f)) * 0.5f * f32(kH);
    return {at.u * f32(kW), (1.0f - at.v) * f32(kH), r};
}

// The summed luma (0..255 a pixel) the pass added inside a rectangle, and the most a pixel there lost.
struct Added {
    i64 sum = 0;
    int darkest = 0;  // the largest decrease (should be 0: the pass only adds)
};

Added added(const Array<u8>& on, const Array<u8>& off, int x0, int y0, int x1, int y1) {
    Added a;
    for (int y = std::max(y0, 0); y < std::min(y1, int(kH)); ++y)
        for (int x = std::max(x0, 0); x < std::min(x1, int(kW)); ++x) {
            const usize p = usize(y) * kW + usize(x);
            const int d = luma8(on, p) - luma8(off, p);
            a.sum += d;
            a.darkest = std::max(a.darkest, -d);
        }
    return a;
}

// ---- running a fragment stage alone
// ----------------------------------------------------------------------------------

// f32 to IEEE half for the values the direct cases upload (normal numbers and 0; round to nearest even).
u16 to_half(f32 v) {
    u32 b;
    std::memcpy(&b, &v, 4);
    const u32 sign = (b >> 16) & 0x8000u;
    const i32 e = i32((b >> 23) & 0xffu) - 127 + 15;
    u32 m = b & 0x7fffffu;
    if (e <= 0) return u16(sign);  // zero (the cases upload no subnormals)
    if (e >= 31) return u16(sign | 0x7c00u);
    u32 h = sign | (u32(e) << 10) | (m >> 13);
    m &= 0x1fffu;
    if (m > 0x1000u || (m == 0x1000u && (h & 1u))) ++h;
    return u16(h);
}

rhi::TextureHandle half_texture(Fixture3D& f, u32 w, u32 h, const std::function<math::vec4(u32, u32)>& texel) {
    Array<u16> data;
    data.resize(usize(w) * h * 4);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const math::vec4 c = texel(x, y);  // row 0 is sampled at v = 0
            const usize i = (usize(y) * w + x) * 4;
            data[i] = to_half(c.x);
            data[i + 1] = to_half(c.y);
            data[i + 2] = to_half(c.z);
            data[i + 3] = to_half(c.w);
        }
    rhi::TextureDesc d;
    d.size = {w, h};
    d.format = rhi::TextureFormat::RGBA16F;
    d.usage = rhi::TextureUsage::Sampled;
    d.debug_name = "test.shafts.input";
    return f.device->create_texture(d, Span<const byte>(reinterpret_cast<const byte*>(data.data()), data.size() * 2));
}

// Reads a float texture's red channel (or its luminance) bit for bit: a pass writes the float's bits as four bytes.
constexpr const char* kEncodeFS = R"(#version 450 core
layout(binding = 6) uniform sampler2D u_src;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec3 c = texelFetch(u_src, ivec2(gl_FragCoord.xy), 0).rgb;
    o_color = unpackUnorm4x8(floatBitsToUint(c.r));
}
)";
constexpr const char* kEncodeLumFS = R"(#version 450 core
layout(binding = 6) uniform sampler2D u_src;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    vec3 c = texelFetch(u_src, ivec2(gl_FragCoord.xy), 0).rgb;
    o_color = unpackUnorm4x8(floatBitsToUint(dot(c, vec3(0.2126, 0.7152, 0.0722))));
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

// The floats `src` holds (red or luminance), read back through kEncodeFS / kEncodeLumFS; rows top first (read_pixels).
Array<f32> read_floats(Fixture3D& f, rhi::TextureHandle src, bool lum) {
    const rhi::Extent2D size = f.device->texture_size(src);
    const rhi::PipelineHandle pipe = fullscreen_pipe(f, lum ? kEncodeLumFS : kEncodeFS, "test.shafts.encode");
    rhi::TextureDesc td;
    td.size = size;
    td.format = rhi::TextureFormat::RGBA8_UNorm;
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.shafts.encode";
    const rhi::TextureHandle rt = f.device->create_texture(td);
    rhi::SamplerDesc sd;
    sd.min_filter = sd.mag_filter = rhi::Filter::Nearest;
    const rhi::SamplerHandle point = f.device->create_sampler(sd);
    f.device->begin_frame(f.frame++);
    rhi::CommandBuffer cmd;
    rhi::PassAttachment o;
    o.texture = rt;
    rhi::RenderPassDesc p;
    p.colors = Span<const rhi::PassAttachment>(&o, 1);
    p.debug_name = "test.shafts.encode";
    cmd.begin_pass(p);
    cmd.bind_pipeline(pipe);
    cmd.bind_texture(rhi::bank::kAlbedo, src, point);
    cmd.draw_non_indexed(3);
    cmd.end_pass();
    f.device->submit(cmd);
    Array<u8> px;
    px.resize(usize(size.width) * size.height * 4);
    REQUIRE(f.device->read_pixels(rt, rhi::Rect2D{0, 0, size.width, size.height}, px.span()).has_value());
    f.device->end_frame();
    CHECK(f.device->validation_error_count() == 0);
    Array<f32> out;
    out.resize(usize(size.width) * size.height);
    for (usize i = 0; i < out.size(); ++i) {
        const u32 bits =
            u32(px[i * 4]) | (u32(px[i * 4 + 1]) << 8) | (u32(px[i * 4 + 2]) << 16) | (u32(px[i * 4 + 3]) << 24);
        std::memcpy(&out[i], &bits, 4);
    }
    f.device->destroy_texture(rt);
    f.device->destroy_pipeline(pipe);
    f.device->destroy_sampler(point);
    return out;
}

// ShaftsU as the shader declares it (forward3d.cpp's mirror is private to the renderer).
struct ShaftsBlock {
    f32 sun[4];    // uv xy, aspect, length
    f32 shape[4];  // per-sample factor, threshold, cap, samples
    f32 color[4];  // rgb, seed
    f32 comp[4];   // scatter_m
};
static_assert(sizeof(ShaftsBlock) == shaders3d::kShaftsUBytes, "ShaftsBlock mirrors ShaftsU");

// kShaftsFS alone: src and gbuf (64 x 64) into a 32 x 32 RGBA16F target with `u`; returns the target's red channel,
// top row first.
Array<f32> run_accumulate(Fixture3D& f, rhi::TextureHandle src, rhi::TextureHandle gbuf, const ShaftsBlock& u) {
    const rhi::PipelineHandle pipe = fullscreen_pipe(f, shaders3d::kShaftsFS, "sun_shafts.frag");
    rhi::TextureDesc td;
    td.size = {32, 32};
    td.format = rhi::TextureFormat::RGBA16F;
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.shafts.rays";
    const rhi::TextureHandle rt = f.device->create_texture(td);
    rhi::SamplerDesc lin;
    const rhi::SamplerHandle linear = f.device->create_sampler(lin);
    rhi::SamplerDesc pd;
    pd.min_filter = pd.mag_filter = rhi::Filter::Nearest;
    const rhi::SamplerHandle point = f.device->create_sampler(pd);
    f.device->begin_frame(f.frame++);
    rhi::CommandBuffer cmd;
    rhi::PassAttachment o;
    o.texture = rt;
    rhi::RenderPassDesc p;
    p.colors = Span<const rhi::PassAttachment>(&o, 1);
    p.debug_name = "test.shafts.accumulate";
    cmd.begin_pass(p);
    cmd.bind_pipeline(pipe);
    cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(u));
    cmd.bind_texture(rhi::bank::kAlbedo, src, linear);
    cmd.bind_texture(rhi::bank::kNormal, gbuf, point);
    cmd.draw_non_indexed(3);
    cmd.end_pass();
    f.device->submit(cmd);
    f.device->end_frame();
    CHECK(f.device->validation_error_count() == 0);
    Array<f32> out = read_floats(f, rt, false);
    f.device->destroy_texture(rt);
    f.device->destroy_pipeline(pipe);
    f.device->destroy_sampler(linear);
    f.device->destroy_sampler(point);
    return out;
}

ShaftsBlock block(f32 sun_u, f32 sun_v, u32 samples, f32 color, f32 cap, f32 threshold = 1.0f, f32 decay = 0.97f,
                  f32 length = 0.85f) {
    f32 per = decay;
    for (u32 n = samples; n < 32u; n *= 2u) per *= per;  // record_sun_shafts' per-sample factor
    ShaftsBlock u{};
    u.sun[0] = sun_u;
    u.sun[1] = sun_v;
    u.sun[2] = 1.0f;
    u.sun[3] = length;
    u.shape[0] = per;
    u.shape[1] = threshold;
    u.shape[2] = cap;
    u.shape[3] = f32(samples);
    u.color[0] = u.color[1] = u.color[2] = color;
    u.color[3] = f32(shaders3d::kShaftsSeed);
    u.comp[0] = 60.0f;
    return u;
}

f32 mean_of(const Array<f32>& v) {
    f64 s = 0.0;
    for (const f32 x : v) s += f64(x);
    return f32(s / f64(v.size()));
}

}  // namespace

TEST_CASE("sun shafts: the measured sky and sun disk radiance sit on either side of the default threshold") {
    // The HDR frame (Forward3D::hdr_target, before every post pass) read back as luminance floats: the gradient sky and
    // the atmosphere's sky, each with the sun at 4, 17 and 33 degrees above the horizon (v 0.26, 0.55, 0.9 of the
    // shafts camera), the disk (the brightest sky pixel), the glow ring (1.5 to 3 disk radii from the sun) and the sky
    // away from the sun (beyond 6 radii). The default threshold must let the disk through (luminance above 2 x the
    // threshold: the mask is smoothstep(t, 2t, lum)) and keep the sky away from the sun under it (mask 0).
    const f32 threshold = SunShafts{}.threshold;
    for (const bool atmosphere : {false, true}) {
        for (const f32 sun_v : {0.26f, kShaftsSunV, 0.9f}) {
            ShaftsFixture h;
            const SunAt at{0.5f, sun_v};
            h.draw(nullptr, at, false, [&](Scene3D& s) {
                if (atmosphere) {
                    Atmosphere a;
                    a.enabled = true;
                    s.set_atmosphere(a);
                }
            });
            const rhi::TextureHandle hdr = h.f.renderer.forward_3d().hdr_target();
            REQUIRE(h.f.device->texture_size(hdr) == rhi::Extent2D{kW, kH});
            const Array<f32> lum = read_floats(h.f, hdr, true);
            const SunPx s = sun_px(at);
            f32 disk = 0.0f, glow = 0.0f, sky = 0.0f;
            for (usize p = 0; p < lum.size(); ++p) {
                const usize row = p / kW;
                const f32 x = f32(p % kW) + 0.5f, y = f32(row) + 0.5f;
                const f32 d = std::sqrt((x - s.x) * (x - s.x) + (y - s.y) * (y - s.y)) / s.r;
                if (y > 0.75f * f32(kH)) continue;  // the ground and the horizon band below the sky
                if (d < 1.0f)
                    disk = std::max(disk, lum[p]);
                else if (d > 1.5f && d < 3.0f)
                    glow = std::max(glow, lum[p]);
                else if (d > 6.0f)
                    sky = std::max(sky, lum[p]);
            }
            MESSAGE("atmosphere " << int(atmosphere) << " (0: the gradient sky), sun at v " << sun_v << ": disk "
                                  << disk << ", glow ring " << glow << ", sky beyond 6 radii " << sky << " (threshold "
                                  << threshold << ")");
            CHECK(disk > 2.0f * threshold);
            CHECK(sky < threshold);
        }
    }
}

TEST_CASE(
    "sun shafts: off, the Low tier, r.sun_shafts 0, a sun below the horizon or behind the camera record nothing") {
    ShaftsFixture h;
    const Shot plain = h.draw(nullptr);
    CHECK_FALSE(plain.stats.sun_shafts);
    CHECK(plain.stats.sun_shafts_strength == 0.0f);
    const SunShafts on = sun_shafts_setting();
    SunShafts off = on;
    off.enabled = false;
    SunShafts dark = on;
    dark.intensity = 0.0f;
    SunShafts hidden = on;
    hidden.visibility = 0.0f;
    for (const SunShafts* sh : {&off, &dark, &hidden}) {
        const Shot s = h.draw(sh);
        CHECK_FALSE(s.stats.sun_shafts);
        CHECK(same(s.px, plain.px));
    }
    for (const auto& [name, value] : {std::pair{"r.quality", "low"}, std::pair{"r.sun_shafts", "0"}}) {
        CvarScope scope(name, value);
        const Shot base = h.draw(nullptr);
        const Shot s = h.draw(&on);
        CHECK_FALSE(s.stats.sun_shafts);
        CHECK(same(s.px, base.px));
    }
    // The sun below the horizon (on screen, near the bottom), behind the camera (above the horizon) and far off the
    // right border: the frame with the same sun and no shafts, bit for bit.
    SunAt below;
    below.explicit_dir = true;
    below.dir = {0.1f, -0.05f, -1.0f};
    SunAt behind;
    behind.explicit_dir = true;
    behind.dir = {0.0f, 0.3f, 1.0f};
    SunAt aside;
    aside.explicit_dir = true;
    aside.dir = {1.0f, 0.2f, -0.2f};
    for (const SunAt& at : {below, behind, aside}) {
        const Shot base = h.draw(nullptr, at);
        const Shot s = h.draw(&on, at);
        CHECK_FALSE(s.stats.sun_shafts);
        CHECK(same(s.px, base.px));
    }
    // The sun in front: the pass runs (the cases below look at what it draws).
    const Shot s = h.draw(&on);
    CHECK(s.stats.sun_shafts);
    CHECK(s.stats.sun_shafts_strength > 0.3f);
}

TEST_CASE("sun shafts: the pass adds light around the sun and only there; the same frame twice is the same") {
    ShaftsFixture h;
    const SunShafts on = sun_shafts_setting();
    const Shot plain = h.draw(nullptr);
    const Shot lit = h.draw(&on);
    REQUIRE(lit.stats.sun_shafts);
    const Shot again = h.draw(&on);
    CHECK(same(lit.px, again.px));
    const SunPx s = sun_px();
    MESSAGE("sun at pixel " << s.x << ", " << s.y << ", disk radius " << s.r << " px");
    // The walk reaches the disk from at most r / (1 - length) away (6.7 radii at the default length 0.85).
    i64 near_sum = 0, ring = 0, far_changed = 0, near_n = 0, ring_n = 0;
    int darkest = 0, brightest_near = 0;
    u32 near_lit = 0;
    for (usize p = 0; p < usize(kW) * kH; ++p) {
        const usize row = p / kW;
        const f32 x = f32(p % kW) + 0.5f, y = f32(row) + 0.5f;
        const f32 d = std::sqrt((x - s.x) * (x - s.x) + (y - s.y) * (y - s.y)) / s.r;
        const int diff = luma8(lit.px, p) - luma8(plain.px, p);
        darkest = std::max(darkest, -diff);
        if (d > 1.2f && d < 4.0f) {
            near_sum += diff;
            ++near_n;
            near_lit += diff > 4 ? 1u : 0u;
            brightest_near = std::max(brightest_near, diff);
        } else if (d >= 4.0f && d < 6.0f) {
            ring += diff;
            ++ring_n;
        } else if (d > 9.0f) {
            far_changed += diff != 0 ? 1 : 0;
        }
    }
    MESSAGE("added luma within 1.2..4 radii " << near_sum << " (" << near_lit << " pixels by more than 4, the most "
                                              << brightest_near << "), within 4..6 radii " << ring
                                              << "; pixels changed beyond 9 radii " << far_changed << ", most darkened "
                                              << darkest);
    CHECK(darkest == 0);
    CHECK(near_lit > 100);
    CHECK(f64(near_sum) / f64(near_n) > 1.5 * f64(ring) / f64(ring_n));  // brighter per pixel nearer the sun
    CHECK(ring > 0);
    CHECK(far_changed == 0);
}

TEST_CASE("sun shafts: a column over half of the disk leaves its side dark and lights the open side") {
    ShaftsFixture h;
    const SunShafts on = sun_shafts_setting();
    const Shot plain = h.draw(nullptr, {}, true);
    const Shot lit = h.draw(&on, {}, true);
    REQUIRE(lit.stats.sun_shafts);
    const SunPx s = sun_px();
    // The column covers the disk's left half and 1.4 radii of sky left of the sun's centre; the band of rows through
    // the sun, from 2 to 5 radii out on either side.
    const int y0 = int(s.y - 0.8f * s.r), y1 = int(s.y + 0.8f * s.r);
    const Added left = added(lit.px, plain.px, int(s.x - 5.0f * s.r), y0, int(s.x - 2.0f * s.r), y1);
    const Added right = added(lit.px, plain.px, int(s.x + 2.0f * s.r), y0, int(s.x + 5.0f * s.r), y1);
    MESSAGE("added luma: left (behind the column) " << left.sum << ", right (open) " << right.sum);
    CHECK(left.darkest == 0);
    CHECK(right.darkest == 0);
    CHECK(right.sum > 200);
    CHECK(left.sum * 5 < right.sum);
    // Without the column both sides get about the same.
    const Shot plain_open = h.draw(nullptr);
    const Shot open = h.draw(&on);
    const Added open_left = added(open.px, plain_open.px, int(s.x - 5.0f * s.r), y0, int(s.x - 2.0f * s.r), y1);
    const Added open_right = added(open.px, plain_open.px, int(s.x + 2.0f * s.r), y0, int(s.x + 5.0f * s.r), y1);
    MESSAGE("without the column: left " << open_left.sum << ", right " << open_right.sum);
    CHECK(open_left.sum * 2 > open_right.sum);
    CHECK(open_right.sum * 2 > open_left.sum);
}

TEST_CASE(
    "sun shafts: a bright wall over the whole sky gives no rays (geometry never radiates); a cloud deck dims them") {
    ShaftsFixture h;
    const SunShafts on = sun_shafts_setting();
    // A white wall 4 m in front of the camera across the whole frame, lit by a strong sun from behind the camera's
    // right (radiance well above the threshold), the sun itself in front: the HDR mask is bright everywhere, the
    // prepass says geometry everywhere.
    const auto wall = [&](Scene3D& s) {
        DirectionalLight sun;
        sun.direction = {-0.3f, -0.4f, -1.0f};  // light travelling into the frame: the wall's face is lit
        sun.intensity = 40.0f;
        sun.shadows = false;
        s.set_light(sun);
        const View3D& v = s.view();
        const vec3 fwd = math::normalize(vec3{-v.view.c[0][2], -v.view.c[1][2], -v.view.c[2][2]});
        const vec3 x = math::normalize(math::cross(fwd, vec3{0.0f, 1.0f, 0.0f}));
        const vec3 y = math::cross(x, fwd);
        const vec3 c = v.position + fwd * 4.0f;
        MeshInstance m;
        m.mesh = 1;
        const vec3 cols[4] = {x * 20.0f, y * 20.0f, -fwd * 0.2f, c - y * 10.0f};
        for (u32 k = 0; k < 4; ++k) {
            m.world.c[k][0] = cols[k].x;
            m.world.c[k][1] = cols[k].y;
            m.world.c[k][2] = cols[k].z;
        }
        s.add(m);
    };
    // The sun in front of the camera but its light comes from behind (the wall's lit face): only the shafts read the
    // light's direction as the sun's, so place the shafts' sun by the light. Here the light travels into the frame, so
    // the sun (toward = -direction) is behind the camera; instead keep the sun in front and light the wall with its
    // ambient: a sky-coloured ambient of 30.
    const auto wall_front = [&](Scene3D& s) {
        wall(s);
        DirectionalLight sun = s.light();
        sun.direction = -lens_flare_toward(s.view(), kShaftsSunU, kShaftsSunV);
        sun.ambient_sky = {30.0f, 30.0f, 30.0f};
        sun.ambient_ground = {30.0f, 30.0f, 30.0f};
        s.set_light(sun);
        Sky sky;
        sky.ibl = false;
        s.set_sky(sky);
    };
    const Shot plain = h.draw(nullptr, {}, false, wall_front);
    const Shot lit = h.draw(&on, {}, false, wall_front);
    CHECK(lit.stats.sun_shafts);  // the sun is in front and above the horizon: the pass runs
    const rhi::TextureHandle hdr = h.f.renderer.forward_3d().hdr_target();
    const Array<f32> lum = read_floats(h.f, hdr, true);
    f32 lowest = 1e30f;
    for (const f32 l : lum) lowest = std::min(lowest, l);
    MESSAGE("the wall's darkest HDR luminance " << lowest);
    CHECK(lowest > 2.0f * on.threshold);  // the whole frame would pass the brightness mask alone
    CHECK(same(lit.px, plain.px));

    // A full cloud deck in front of the sun: the disk is dimmed in the HDR frame, so the rays all but vanish.
    const auto deck = [&](Scene3D& s) {
        Clouds c;
        c.coverage = 1.0f;
        c.density = 1.0f;
        s.set_clouds(c);
    };
    const SunPx sp = sun_px();
    const int x0 = int(sp.x - 5.0f * sp.r), x1 = int(sp.x + 5.0f * sp.r), y0 = int(sp.y - 5.0f * sp.r),
              y1 = int(sp.y + 5.0f * sp.r);
    const Shot clear0 = h.draw(nullptr);
    const Shot clear1 = h.draw(&on);
    const Shot cloud0 = h.draw(nullptr, {}, false, deck);
    const Shot cloud1 = h.draw(&on, {}, false, deck);
    CHECK(cloud1.stats.sun_shafts);
    const Added clear = added(clear1.px, clear0.px, x0, y0, x1, y1);
    const Added cloudy = added(cloud1.px, cloud0.px, x0, y0, x1, y1);
    MESSAGE("added luma around the sun: clear " << clear.sum << ", under a full cloud deck " << cloudy.sum);
    CHECK(clear.sum > 500);
    CHECK(cloudy.sum * 10 < clear.sum);
}

TEST_CASE("sun shafts: 16 (Medium) and 32 (High) samples agree in brightness") {
    ShaftsFixture h;
    const SunShafts on = sun_shafts_setting();
    const SunPx s = sun_px();
    const int x0 = int(s.x - 7.0f * s.r), x1 = int(s.x + 7.0f * s.r), y0 = int(s.y - 7.0f * s.r),
              y1 = int(s.y + 7.0f * s.r);
    i64 sums[2] = {0, 0};
    Array<u8> frames[2];
    for (u32 k = 0; k < 2; ++k) {
        CvarScope scope("r.quality", k == 0 ? "medium" : "high");
        const Shot base = h.draw(nullptr);
        const Shot lit = h.draw(&on);
        REQUIRE(lit.stats.sun_shafts);
        sums[k] = added(lit.px, base.px, x0, y0, x1, y1).sum;
        frames[k] = lit.px;
    }
    MESSAGE("added luma around the sun: Medium (16) " << sums[0] << ", High (32) " << sums[1]);
    CHECK(sums[0] > 500);
    CHECK(f64(sums[0]) > 0.85 * f64(sums[1]));
    CHECK(f64(sums[0]) < 1.15 * f64(sums[1]));
    CHECK_FALSE(same(frames[0], frames[1]));  // the same brightness, not the same picture
}

TEST_CASE("sun shafts: with TAA the next frame holds no rays; the volumetric fog backs the strength off to 0.4") {
    const SunShafts on = sun_shafts_setting();
    auto two_frames = [&](bool shafts_first) {
        ShaftsFixture h;
        const auto taa = [](Scene3D& s) {
            PostFx post = s.post();
            post.taa = true;
            s.set_post(post);
        };
        const Shot first = h.draw(shafts_first ? &on : nullptr, {}, false, taa);
        CHECK(first.stats.taa);
        CHECK(first.stats.sun_shafts == shafts_first);
        const Shot second = h.draw(nullptr, {}, false, taa);
        CHECK(second.stats.taa_history);
        CHECK_FALSE(second.stats.sun_shafts);
        return second.px;
    };
    CHECK(same(two_frames(true), two_frames(false)));

    ShaftsFixture h;
    const Shot clear = h.draw(&on);
    const Shot vol = h.draw(&on, {}, false, [](Scene3D& s) {
        Volumetrics v;
        v.enabled = true;
        v.density = 0.01f;
        s.set_volumetrics(v);
    });
    REQUIRE(vol.stats.volumetrics);
    REQUIRE(vol.stats.sun_shafts);
    MESSAGE("strength without the volume " << clear.stats.sun_shafts_strength << ", with it "
                                           << vol.stats.sun_shafts_strength);
    CHECK(vol.stats.sun_shafts_strength ==
          doctest::Approx(kSunShaftsVolumeShare * clear.stats.sun_shafts_strength).epsilon(1e-5));
}

TEST_CASE("sun shafts: a render-to-texture view records no shafts") {
    ShaftsFixture h;
    const SunShafts on = sun_shafts_setting();
    for (const bool linear : {false, true}) {
        h.f.device->begin_frame(h.f.frame++);
        rhi::CommandBuffer cmd;
        Scene3D scene(h.f.renderer.meshes(), mem::heap());
        scene.begin(orbit_camera(h.f.look_at, 0.0f, math::radians(h.f.pitch_deg), 7.0f), {kW, kH});
        MeshInstance ground;
        ground.mesh = 2;
        scene.add(ground);
        sun_shafts_scene(scene, lens_flare_toward(scene.view(), kShaftsSunU, kShaftsSunV), &on);
        scene.build();
        Forward3D::ViewOutput out;
        out.slot = 1;
        out.linear_output = linear;
        h.f.renderer.record_3d(cmd, scene, h.f.target, out);
        CHECK(h.f.renderer.forward_3d().last_stats().sun_shafts == !linear);
        h.f.device->submit(cmd);
        h.f.device->end_frame();
        CHECK(h.f.device->validation_error_count() == 0);
    }
}

TEST_CASE("sun shafts: after a resize a window view recorded after a render-texture view gets half-resolution shafts") {
    ShaftsFixture h;
    const SunShafts on = sun_shafts_setting();
    const rhi::Extent2D small{128, 96};
    // One frame of two views: a render-texture view first (the frame's first, it draws no shafts), then the window view
    // at `small` (not first) with the edge outline on and TAA off, so a trans_src_ fallback would draw nothing.
    auto frame = [&](const SunShafts* shafts, Forward3DStats& stats) {
        h.f.device->begin_frame(h.f.frame++);
        rhi::CommandBuffer cmd;
        for (const bool rt : {true, false}) {
            const rhi::Extent2D size = rt ? rhi::Extent2D{kW, kH} : small;
            Scene3D scene(h.f.renderer.meshes(), mem::heap());
            scene.begin(orbit_camera(h.f.look_at, 0.0f, math::radians(h.f.pitch_deg), 7.0f), size);
            MeshInstance ground;
            ground.mesh = 2;
            scene.add(ground);
            sun_shafts_scene(scene, lens_flare_toward(scene.view(), kShaftsSunU, kShaftsSunV), shafts);
            PostFx post = scene.post();
            post.taa = false;
            post.outline = outline_setting();
            scene.set_post(post);
            scene.build();
            Forward3D::ViewOutput out;
            out.slot = rt ? 1u : 0u;
            out.first = rt;
            out.linear_output = rt;
            if (!rt) out.rect = rhi::Rect2D{0, 0, size.width, size.height};
            h.f.renderer.record_3d(cmd, scene, h.f.target, out);
            stats = h.f.renderer.forward_3d().last_stats();
        }
        h.f.device->submit(cmd);
        REQUIRE(h.f.device->read_pixels(h.f.target, rhi::Rect2D{0, 0, kW, kH}, h.f.pixels.span()).has_value());
        h.f.device->end_frame();
        CHECK(h.f.device->validation_error_count() == 0);
        return h.f.pixels;
    };
    // The window at its first size makes shafts_buf_ at 80 x 60.
    const Shot first = h.draw(&on);
    REQUIRE(first.stats.sun_shafts);
    Forward3DStats with_stats, without_stats;
    const Array<u8> with = frame(&on, with_stats);
    const Array<u8> without = frame(nullptr, without_stats);
    CHECK(with_stats.sun_shafts);
    CHECK_FALSE(without_stats.sun_shafts);
    const SunPx sp = sun_px();
    const int r = int(sp.r * 8.0f);
    const Added a = added(with, without, int(sp.x) - r, int(sp.y) - r, int(sp.x) + r, int(sp.y) + r);
    MESSAGE("light the shafts added around the sun after the resize: " << a.sum);
    CHECK(a.sum > 500);
    CHECK(a.darkest == 0);
}

TEST_CASE("sun shafts: the sun sweeping across the border, through the horizon and behind the camera does not pop") {
    // Each sweep moves the sun in small steps and measures the added light (the frame with the pass minus the frame
    // without it, summed over the whole frame); the largest change between adjacent steps stays a small share of the
    // largest added light, so nothing appears or vanishes in one step. The source is the disk itself, so the rays leave
    // with it as it crosses the border (about two disk radii, 10 pixels, of sun motion): the border sweep moves the sun
    // a pixel a step. With the fixture's 45-degree field of view the disk is off screen long before the sun nears the
    // camera's side plane (where the facing fade acts), so the sweep behind the camera starts just past the border and
    // checks that no step adds anything and that the pass stops recording once the sun is behind.
    ShaftsFixture h;
    const SunShafts on = sun_shafts_setting();
    auto added_at = [&](const SunAt& at, bool* recorded = nullptr) {
        const Shot base = h.draw(nullptr, at);
        const Shot lit = h.draw(&on, at);
        if (recorded) *recorded = lit.stats.sun_shafts;
        return added(lit.px, base.px, 0, 0, int(kW), int(kH)).sum;
    };
    struct Sweep {
        const char* name;
        u32 steps;
        std::function<SunAt(f32)> at;  // t in 0..1
    };
    const Sweep sweeps[2] = {
        {"across the right border", 32, [](f32 t) { return SunAt{0.93f + 0.2f * t, kShaftsSunV}; }},  // 1 px a step
        {"down through the horizon", 24,
         [](f32 t) {
             SunAt a;
             a.explicit_dir = true;
             const f32 e = 0.10f - 0.16f * t;  // elevation, radians: 0.0067 a step
             a.dir = {0.08f, std::sin(e), -std::cos(e)};
             return a;
         }},
    };
    for (const Sweep& sw : sweeps) {
        i64 most = 0, prev = 0, largest_step = 0;
        for (u32 i = 0; i <= sw.steps; ++i) {
            const i64 a = added_at(sw.at(f32(i) / f32(sw.steps)));
            most = std::max(most, a);
            if (i > 0) largest_step = std::max(largest_step, std::abs(a - prev));
            prev = a;
        }
        MESSAGE(sw.name << ": most added " << most << ", largest step " << largest_step << ", last " << prev);
        CHECK(most > 500);
        CHECK(largest_step * 4 < most);
        CHECK(prev == 0);  // each sweep ends where nothing is added
    }
    i64 behind_most = 0;
    bool recorded = true;
    for (u32 i = 0; i <= 12; ++i) {
        const f32 yaw = 0.6f + 2.4f * f32(i) / 12.0f;  // just past the right border to almost straight behind
        SunAt a;
        a.explicit_dir = true;
        a.dir = {std::sin(yaw), 0.15f, -std::cos(yaw)};
        behind_most = std::max(behind_most, added_at(a, &recorded));
    }
    MESSAGE("around behind the camera: most added " << behind_most);
    CHECK(behind_most == 0);
    CHECK_FALSE(recorded);
}

TEST_CASE("sun shafts: the shader's hash is sun_shafts_hash bit for bit") {
    // kShaftsFS's ss_hash (MCE_GLSL_SUN_SHAFTS_HASH) run alone: one row of 64 pixels, each hashing inputs made from its
    // column (column 0 the all-ones case of the CPU goldens), written as four bytes and read back.
    Fixture3D f;
    constexpr const char* kFS = "#version 450 core\n" MCE_GLSL_SUN_SHAFTS_HASH R"(
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    uint x = uint(gl_FragCoord.x);
    uint h = x == 0u ? ss_hash(0xffffffffu, 0xffffffffu, 0xffffffffu)
                     : ss_hash(x * 0x9e3779b9u, (x ^ 0x5bd1e995u) * 0x01000193u, x * 0x27d4eb2fu + 7u);
    o_color = unpackUnorm4x8(h);
}
)";
    const rhi::PipelineHandle pipe = fullscreen_pipe(f, kFS, "shafts_hash_test.frag");
    constexpr u32 kN = 64;
    rhi::TextureDesc td;
    td.size = {kN, 1};
    td.format = rhi::TextureFormat::RGBA8_UNorm;
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.shafts_hash";
    const rhi::TextureHandle rt = f.device->create_texture(td);
    f.device->begin_frame(f.frame++);
    rhi::CommandBuffer cmd;
    rhi::PassAttachment o;
    o.texture = rt;
    rhi::RenderPassDesc p;
    p.colors = Span<const rhi::PassAttachment>(&o, 1);
    p.debug_name = "test.shafts_hash";
    cmd.begin_pass(p);
    cmd.bind_pipeline(pipe);
    cmd.draw_non_indexed(3);
    cmd.end_pass();
    f.device->submit(cmd);
    Array<u8> px;
    px.resize(usize(kN) * 4);
    REQUIRE(f.device->read_pixels(rt, rhi::Rect2D{0, 0, kN, 1}, px.span()).has_value());
    f.device->end_frame();
    CHECK(f.device->validation_error_count() == 0);
    u32 match = 0;
    for (u32 x = 0; x < kN; ++x) {
        const u32 want = x == 0
                             ? sun_shafts_hash(0xffffffffu, 0xffffffffu, 0xffffffffu)
                             : sun_shafts_hash(x * 0x9e3779b9u, (x ^ 0x5bd1e995u) * 0x01000193u, x * 0x27d4eb2fu + 7u);
        const u32 got =
            u32(px[x * 4]) | (u32(px[x * 4 + 1]) << 8) | (u32(px[x * 4 + 2]) << 16) | (u32(px[x * 4 + 3]) << 24);
        INFO("column " << x);
        CHECK(got == want);
        match += got == want ? 1u : 0u;
    }
    CHECK(match == kN);
    CHECK(sun_shafts_hash(0xffffffffu, 0xffffffffu, 0xffffffffu) == 0x828050a8u);  // the CPU golden it rests on
    f.device->destroy_texture(rt);
    f.device->destroy_pipeline(pipe);
}

TEST_CASE(
    "sun shafts: the accumulation run alone: the soft clamp, 16 and 32 samples, the border rule and the sky gate") {
    Fixture3D f;
    // The bright square: an 8 x 8 texel sun around uv (0.5, 0.5) at luminance 1000 (white), sky 0.5 elsewhere.
    const rhi::TextureHandle src = half_texture(f, 64, 64, [](u32 x, u32 y) {
        const bool sun = x >= 28 && x < 36 && y >= 28 && y < 36;
        const f32 c = sun ? 1000.0f : 0.5f;
        return math::vec4{c, c, c, 1.0f};
    });
    const rhi::TextureHandle sky = half_texture(f, 64, 64, [](u32, u32) { return math::vec4{0.0f, 0.0f, 1.0f, 0.0f}; });
    const rhi::TextureHandle wall =
        half_texture(f, 64, 64, [](u32, u32) { return math::vec4{0.0f, 0.0f, 1.0f, 5.0f}; });

    // The soft clamp: everything bright and sky, the sun inside the frame (every walk stays inside 0..1 and every
    // sample is a full-weight source: lum / threshold capped at 16), so every texel is soft_clamp(color * 16, cap)
    // exactly.
    const rhi::TextureHandle bright =
        half_texture(f, 64, 64, [](u32, u32) { return math::vec4{1000.0f, 1000.0f, 1000.0f, 1.0f}; });
    for (const auto& [color, cap] :
         {std::pair{0.01f, 1.5f}, std::pair{0.1f, 1.5f}, std::pair{100.0f, 0.25f}, std::pair{1.0f, 8.0f}}) {
        const Array<f32> r = run_accumulate(f, bright, sky, block(0.5f, 0.5f, 32, color, cap));
        const f32 want = sun_shafts_soft_clamp(color * 16.0f, cap);
        f32 lo = 1e30f, hi = -1e30f;
        for (const f32 v : r) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        MESSAGE("colour " << color << ", cap " << cap << ": want " << want << ", got " << lo << " .. " << hi);
        CHECK(lo == doctest::Approx(want).epsilon(2e-3));
        CHECK(hi == doctest::Approx(want).epsilon(2e-3));
        CHECK(hi <= cap);
    }

    // 16 and 32 samples: the same mean (a tolerance for the coarser sampling), a different picture.
    const Array<f32> r16 = run_accumulate(f, src, sky, block(0.5f, 0.5f, 16, 0.02f, 8.0f));
    const Array<f32> r32 = run_accumulate(f, src, sky, block(0.5f, 0.5f, 32, 0.02f, 8.0f));
    const f32 m16 = mean_of(r16), m32 = mean_of(r32);
    MESSAGE("mean rays: 16 samples " << m16 << ", 32 samples " << m32);
    CHECK(m32 > 0.01f);
    CHECK(m16 == doctest::Approx(m32).epsilon(0.1));
    bool differ = false;
    for (usize i = 0; i < r16.size(); ++i) differ = differ || r16[i] != r32[i];
    CHECK(differ);

    // Geometry everywhere (prepass depth 5 m): nothing radiates, every texel is exactly 0.
    const Array<f32> blocked = run_accumulate(f, src, wall, block(0.5f, 0.5f, 32, 0.02f, 8.0f));
    f32 most = 0.0f;
    for (const f32 v : blocked) most = std::max(most, v);
    CHECK(most == 0.0f);

    // The border rule: only the last column is bright and the sun is far off the right (uv 2.0): a walk leaves the
    // frame and its samples outside 0..1 add nothing, so a texel in the middle sees at most a sample or two of the
    // bright column (clamping to the border would give it over half of its samples).
    const rhi::TextureHandle edge = half_texture(f, 64, 64, [](u32 x, u32) {
        const f32 c = x == 63 ? 1000.0f : 0.0f;
        return math::vec4{c, c, c, 1.0f};
    });
    const Array<f32> border = run_accumulate(f, edge, sky, block(2.0f, 0.5f, 32, 1.0f / 16.0f, 8.0f));
    f32 middle = 0.0f;
    for (u32 y = 8; y < 24; ++y)
        for (u32 x = 8; x < 24; ++x) middle = std::max(middle, border[usize(y) * 32 + x]);
    const f32 clamped = sun_shafts_soft_clamp(0.5f, 8.0f);  // what half of the samples on the source would give
    MESSAGE("border rule: the middle's brightest texel " << middle << " (clamping to the border would give over "
                                                         << clamped << ")");
    CHECK(middle < 0.25f * clamped);
    for (const rhi::TextureHandle t : {src, sky, wall, bright, edge}) f.device->destroy_texture(t);
}
