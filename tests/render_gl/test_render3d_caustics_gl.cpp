// M-CAUSTICS 52b against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend: caustics under water, one
// fullscreen pass that rebuilds each pixel's position from the half-resolution opaque prepass, follows the sun up to the
// water plane, reads the procedural web there and adds the sun's light times caustics_term to the pixel, after the cloud
// shadows and before the sun shafts (Forward3D::record_caustics, kCausticsFS).
// The off paths (the setting off, a zero strength, the Low tier, r.caustics 0, no water, a sun under the fade band, below
// the horizon, NaN or zero, a sun without light, a render-to-texture view) draw the frame without the pass bit for bit;
// with the pass only pixels below the plane change and only brighter (the box's part above the water, the water with
// nothing behind it and the sky stay as they are); more depth gives less light; the web moves with the scene clock and is
// bit for bit one period later, and speed 0 holds it; a floating-origin rebase leaves the web on the floor; the light fades
// as the sun sinks toward the horizon; the camera under water still draws it (before the underwater composite); Medium and
// High both draw; with TAA the next frame holds no light; a second window view of another size draws; the exp² fog takes
// its share; and the shader alone agrees with the CPU twin (caustics.h) on a grid of points.
// One 160 x 120 frame per render.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>

#include "fixture3d.h"
#include "mce/convar.h"
#include "mce/render/caustics.h"
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

struct CausticsFixture {
    Fixture3D f;
    Shot draw(const WaterSurface* w, const Caustics* c, f32 time = 0.0f, vec3 toward = kCausticsToward,
              const std::function<void(Scene3D&)>& more = {}) {
        f.render(0.0f, false, [&](Scene3D& s) {
            caustics_scene(s, w, c, time, toward);
            if (more) more(s);
        });
        CHECK(f.device->validation_error_count() == 0);
        return {f.pixels, f.renderer.forward_3d().last_stats(), f.view};
    }
};

// What the pass did to a frame: per pixel the luma it added (on - off; negative = it darkened).
struct Light {
    i64 sum = 0;        // summed luma added
    usize lit = 0;      // pixels brighter by at least 2 luma
    int darkest = 0;    // the largest darkening of any channel (should be 0: the pass only adds)
};

Light light(const Array<u8>& on, const Array<u8>& off) {
    Light l;
    for (usize p = 0; p < usize(kW) * kH; ++p) {
        const int d = luma8(on, p) - luma8(off, p);
        l.sum += d;
        if (d >= 2) ++l.lit;
        for (int c = 0; c < 3; ++c) l.darkest = std::max(l.darkest, int(off[p * 4 + c]) - int(on[p * 4 + c]));
    }
    return l;
}

// What a pixel's view ray hits first, by the CPU: the floor's top (y 0, |x|, |z| <= 6), the box ([-0.5, 0.5] x [0, 1] x
// [-0.5, 0.5]) or nothing. (px, py) in top-row-first pixel coordinates; `hit` the world point.
bool ray_hit(const View3D& view, f32 px, f32 py, vec3& hit) {
    const math::mat4 inv = math::inverse(view.view_proj);
    const f32 nx = px / f32(kW) * 2.0f - 1.0f, ny = 1.0f - py / f32(kH) * 2.0f;
    const math::vec4 a = inv * math::vec4(nx, ny, -1.0f, 1.0f), b = inv * math::vec4(nx, ny, 1.0f, 1.0f);
    const vec3 o{a.x / a.w, a.y / a.w, a.z / a.w}, e{b.x / b.w, b.y / b.w, b.z / b.w};
    const vec3 d = e - o;
    f32 best = 1e30f;
    // The box (slab test).
    f32 t0 = 0.0f, t1 = 1e30f;
    bool box = true;
    const f32 lo[3] = {-0.5f, 0.0f, -0.5f}, hi[3] = {0.5f, 1.0f, 0.5f};
    const f32 oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
    for (int k = 0; k < 3 && box; ++k) {
        if (std::fabs(dd[k]) < 1e-12f) {
            box = oo[k] >= lo[k] && oo[k] <= hi[k];
            continue;
        }
        f32 ta = (lo[k] - oo[k]) / dd[k], tb = (hi[k] - oo[k]) / dd[k];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        box = t0 <= t1;
    }
    if (box) best = t0;
    if (d.y < 0.0f) {
        const f32 t = -o.y / d.y;
        const vec3 g = o + d * t;
        if (std::fabs(g.x) <= 6.0f && std::fabs(g.z) <= 6.0f && t < best) best = t;
    }
    if (best > 1e29f) return false;
    hit = o + d * best;
    return true;
}

// Per pixel: 0 nothing behind (sky, or water with nothing under it), 1 a surface under the water, 2 a surface above it,
// 3 mixed (the class differs within the pixel's 3 x 3 neighbourhood: the half-resolution prepass's block).
Array<u8> classes(const View3D& view, f32 level) {
    Array<u8> one;
    one.resize(usize(kW) * kH);
    for (u32 y = 0; y < kH; ++y)
        for (u32 x = 0; x < kW; ++x) {
            vec3 h;
            u8 c = 0;
            if (ray_hit(view, f32(x) + 0.5f, f32(y) + 0.5f, h)) c = h.y < level - 0.02f ? 1 : (h.y > level + 0.02f ? 2 : 3);
            one[usize(y) * kW + x] = c;
        }
    Array<u8> out;
    out.resize(one.size());
    for (u32 y = 0; y < kH; ++y)
        for (u32 x = 0; x < kW; ++x) {
            const u8 c = one[usize(y) * kW + x];
            u8 r = c;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = int(x) + dx, yy = int(y) + dy;
                    if (xx < 0 || yy < 0 || xx >= int(kW) || yy >= int(kH)) continue;
                    if (one[usize(yy) * kW + usize(xx)] != c) r = 3;
                }
            out[usize(y) * kW + x] = r;
        }
    return out;
}

}  // namespace

TEST_CASE("caustics: the off paths draw the frame without the pass bit for bit") {
    CausticsFixture h;
    const WaterSurface w = caustics_water();
    const Caustics on = caustics_setting();
    const Shot plain = h.draw(&w, nullptr);
    CHECK_FALSE(plain.stats.caustics);
    CHECK(plain.stats.caustics_strength == 0.0f);
    Caustics off = on;
    off.enabled = false;
    Caustics weak = on;
    weak.strength = 0.0f;
    for (const Caustics* c : {&off, &weak}) {
        const Shot s = h.draw(&w, c);
        CHECK_FALSE(s.stats.caustics);
        CHECK(same(s.px, plain.px));
    }
    for (const auto& [name, value] : {std::pair{"r.quality", "low"}, std::pair{"r.caustics", "0"}}) {
        CvarScope scope(name, value);
        const Shot base = h.draw(&w, nullptr);
        const Shot s = h.draw(&w, &on);
        CHECK_FALSE(s.stats.caustics);
        CHECK(same(s.px, base.px));
    }
    // No water: none at all, and a water carrier that is off.
    WaterSurface dry_carrier = w;
    dry_carrier.enabled = false;
    const WaterSurface* const dry = &dry_carrier;
    for (const WaterSurface* ws : {static_cast<const WaterSurface*>(nullptr), dry}) {
        const Shot base = h.draw(ws, nullptr);
        const Shot s = h.draw(ws, &on);
        CHECK_FALSE(s.stats.caustics);
        CHECK(same(s.px, base.px));
    }
    // A sun under the fade band, below the horizon, a NaN and a zero direction: the frame with the same sun, no pass.
    const f32 nan = std::nanf("");
    for (const vec3 toward : {vec3{0.0f, 0.015f, 1.0f}, vec3{0.3f, -0.5f, 0.3f}, vec3{nan, 1.0f, 0.0f},
                              vec3{0.0f, 0.0f, 0.0f}}) {
        const Shot base = h.draw(&w, nullptr, 0.0f, toward);
        const Shot s = h.draw(&w, &on, 0.0f, toward);
        CHECK_FALSE(s.stats.caustics);
        CHECK(same(s.px, base.px));
    }
    // The sun without light (intensity 0): nothing to add.
    const auto dark = [](Scene3D& s) {
        DirectionalLight l = s.light();
        l.intensity = 0.0f;
        s.set_light(l);
    };
    const Shot dark_base = h.draw(&w, nullptr, 0.0f, kCausticsToward, dark);
    const Shot dark_on = h.draw(&w, &on, 0.0f, kCausticsToward, dark);
    CHECK_FALSE(dark_on.stats.caustics);
    CHECK(same(dark_on.px, dark_base.px));
    // On: the pass runs, with the setting's strength.
    const Shot s = h.draw(&w, &on);
    CHECK(s.stats.caustics);
    CHECK(s.stats.caustics_strength == doctest::Approx(1.0f));
    CHECK_FALSE(same(s.px, plain.px));
    // The same frame twice: the same bytes.
    CHECK(same(h.draw(&w, &on).px, s.px));
}

TEST_CASE("caustics: only pixels below the plane change, only brighter; above it, the bare water and the sky stay") {
    CausticsFixture h;
    const WaterSurface w = caustics_water();
    const Caustics on = caustics_setting();
    const Shot off = h.draw(&w, nullptr);
    const Shot lit = h.draw(&w, &on);
    REQUIRE(lit.stats.caustics);
    const Array<u8> cls = classes(lit.view, kCausticsLevel);
    usize count[4] = {}, changed[4] = {};
    for (usize p = 0; p < cls.size(); ++p) {
        ++count[cls[p]];
        changed[cls[p]] += same_px(off.px, lit.px, p) ? 0 : 1;
    }
    MESSAGE("nothing behind: " << count[0] << " px (" << changed[0] << " changed), under water " << count[1] << " ("
                               << changed[1] << "), above " << count[2] << " (" << changed[2] << "), mixed " << count[3]
                               << " (" << changed[3] << ")");
    CHECK(count[0] > usize(kW) * 10);
    CHECK(count[1] > usize(kW) * 30);
    CHECK(count[2] > 30);  // the box's top above the water
    CHECK(changed[0] == 0);
    CHECK(changed[2] == 0);
    CHECK(changed[1] > count[1] / 3);
    const Light l = light(lit.px, off.px);
    CHECK(l.darkest == 0);
    CHECK(l.lit > usize(kW) * 10);
}

TEST_CASE("caustics: more depth gives less light") {
    CausticsFixture h;
    const Caustics on = caustics_setting();
    i64 last = -1;
    for (const f32 level : {4.0f, 2.0f, 0.6f}) {  // deepest first: the floor is 4, 2 and 0.6 m down
        const WaterSurface w = caustics_water(level);
        const Light l = light(h.draw(&w, &on).px, h.draw(&w, nullptr).px);
        MESSAGE("floor " << level << " m under the water: luma added " << l.sum << " over " << l.lit << " pixels");
        CHECK(l.darkest == 0);
        CHECK(l.sum > last);
        last = l.sum;
    }
    // max_depth 0.75 m: the floor 2 m down and the box's top 1 m down get exactly nothing (the pass runs and adds 0).
    Caustics shallow = on;
    shallow.max_depth = 0.75f;
    const WaterSurface w2 = caustics_water(2.0f);
    const Shot base = h.draw(&w2, nullptr);
    const Shot s = h.draw(&w2, &shallow);
    CHECK(s.stats.caustics);
    CHECK(same(s.px, base.px));
}

TEST_CASE("caustics: the web moves with the clock, is bit for bit one period later, and speed 0 holds it") {
    CausticsFixture h;
    const WaterSurface w = caustics_water();
    const Caustics on = caustics_setting();
    const Shot t0 = h.draw(&w, &on, 1.25f);
    const Shot t1 = h.draw(&w, &on, 1.5f);
    const Shot t4 = h.draw(&w, &on, 1.25f + 4.0f);  // kCausticsPeriodTicks at speed 1: 4 s
    const Shot t8 = h.draw(&w, &on, 1.25f + 400.0f);
    CHECK_FALSE(same(t0.px, t1.px));
    CHECK(same(t0.px, t4.px));
    CHECK(same(t0.px, t8.px));
    // Double speed: the period halves.
    Caustics fast = on;
    fast.speed = 2.0f;
    CHECK(same(h.draw(&w, &fast, 0.75f).px, h.draw(&w, &fast, 2.75f).px));
    Caustics still = on;
    still.speed = 0.0f;
    CHECK(same(h.draw(&w, &still, 0.0f).px, h.draw(&w, &still, 1234.5f).px));
}

TEST_CASE("caustics: a floating-origin rebase leaves the web on the floor; an unannounced move slides it") {
    const WaterSurface w = caustics_water();
    const Caustics on = caustics_setting();
    // The light the pass added (on - off luma) per pixel.
    // One renderer per run; the shift is announced on its first frame only (Forward3D keeps the origin it was told).
    auto added = [&](vec3 world_offset, bool announce) {
        CausticsFixture h;
        h.f.world_offset = world_offset;
        const auto shift = [&](Scene3D& s) { s.set_origin_shift(-world_offset); };
        const Shot o = h.draw(&w, nullptr, 2.0f, kCausticsToward, announce ? shift : std::function<void(Scene3D&)>{});
        const Shot l = h.draw(&w, &on, 2.0f, kCausticsToward);
        CHECK(l.stats.caustics);
        Array<int> d;
        d.resize(usize(kW) * kH);
        for (usize p = 0; p < d.size(); ++p) d[p] = luma8(l.px, p) - luma8(o.px, p);
        return d;
    };
    const Array<int> home = added(vec3{0.0f, 0.0f, 0.0f}, false);
    const vec3 shift{-1000.0f, 0.0f, -1000.0f};  // the world moved by -1000 m: 16 water cells, not a whole pattern period
    const Array<int> rebased = added(shift, true);
    const Array<int> unannounced = added(shift, false);
    auto diff = [&](const Array<int>& a) {
        i64 sum = 0, ref = 0;
        for (usize p = 0; p < home.size(); ++p) {
            sum += std::abs(a[p] - home[p]);
            ref += std::abs(home[p]);
        }
        return f64(sum) / f64(std::max<i64>(ref, 1));
    };
    const f64 moved = diff(rebased), slid = diff(unannounced);
    MESSAGE("added light after a 1000 m rebase differs by " << moved * 100.0 << "% of itself, without set_origin_shift "
                                                            << slid * 100.0 << "%");
    CHECK(moved < 0.05);  // float positions at 1000 m (the rest of the frame moves by as much)
    CHECK(slid > 0.5);
    CHECK(slid > 10.0 * moved);
}

TEST_CASE("caustics: the light fades to nothing as the sun sinks toward the horizon") {
    CausticsFixture h;
    const WaterSurface w = caustics_water();
    const Caustics on = caustics_setting();
    i64 last = -1;
    for (const f32 y : {0.019f, 0.05f, 0.1f, 0.2f, 0.6f}) {
        const f32 hz = std::sqrt(1.0f - y * y);
        const vec3 toward{0.6f * hz, y, 0.8f * hz};  // behind the camera and to the right
        const Shot base = h.draw(&w, nullptr, 0.0f, toward);
        const Shot s = h.draw(&w, &on, 0.0f, toward);
        const Light l = light(s.px, base.px);
        MESSAGE("sun elevation sin " << y << ": luma added " << l.sum << " over " << l.lit << " pixels");
        if (y < kCausticsMinSunY) {
            CHECK_FALSE(s.stats.caustics);
            CHECK(same(s.px, base.px));
        }
        CHECK(l.darkest == 0);
        // The fade and the shorter sun path in water both grow with the elevation.
        CHECK(l.sum >= last);
        last = l.sum;
    }
    CHECK(last > 0);
}

TEST_CASE("caustics: the camera under water still draws them, before the underwater composite") {
    CausticsFixture h;
    // A 6 m deep pool, the camera 2.5 m under the surface looking down at the floor.
    const WaterSurface w = caustics_water(6.0f);
    const Caustics on = caustics_setting();
    underwater_place_camera(h.f, vec3{0.0f, 3.5f, 5.0f}, 40.0f);
    const Shot off = h.draw(&w, nullptr);
    const Shot lit = h.draw(&w, &on);
    REQUIRE(lit.stats.caustics);
    CHECK(lit.stats.underwater);
    const Light l = light(lit.px, off.px);
    MESSAGE("under water: luma added " << l.sum << " over " << l.lit << " pixels");
    CHECK(l.darkest <= 1);  // the underwater look's wobble reads its source bilinearly: one step of rounding
    CHECK(l.lit > usize(kW) * 10);
}

TEST_CASE("caustics: Medium (2 octaves) and High (3) both draw, differently") {
    CausticsFixture h;
    const WaterSurface w = caustics_water();
    const Caustics on = caustics_setting();
    Array<u8> frames[2];
    for (u32 k = 0; k < 2; ++k) {
        CvarScope scope("r.quality", k == 0 ? "medium" : "high");
        const Shot s = h.draw(&w, &on);
        CHECK(s.stats.caustics);
        frames[k] = s.px;
        CHECK(light(s.px, h.draw(&w, nullptr).px).lit > usize(kW) * 10);
    }
    CHECK_FALSE(same(frames[0], frames[1]));
}

TEST_CASE("caustics: with TAA the light never enters the history") {
    const WaterSurface w = caustics_water();
    const Caustics on = caustics_setting();
    const auto taa = [](Scene3D& s) {
        PostFx post = s.post();
        post.taa = true;
        s.set_post(post);
    };
    auto frames = [&](bool lit_first) {
        CausticsFixture h;
        for (int i = 0; i < 2; ++i) {
            const Shot s = h.draw(&w, lit_first ? &on : nullptr, 0.0f, kCausticsToward, taa);
            CHECK(s.stats.taa);
            CHECK(s.stats.caustics == lit_first);
        }
        const Shot last = h.draw(&w, nullptr, 0.0f, kCausticsToward, taa);
        CHECK(last.stats.taa_history);
        CHECK_FALSE(last.stats.caustics);
        return last.px;
    };
    CHECK(same(frames(true), frames(false)));
}

TEST_CASE("caustics: a render-to-texture view records none; a second window view of another size draws") {
    CausticsFixture h;
    const WaterSurface w = caustics_water();
    const Caustics on = caustics_setting();
    REQUIRE(h.draw(&w, &on).stats.caustics);
    auto frame = [&](const Caustics* c, Forward3DStats stats[2]) {
        h.f.device->begin_frame(h.f.frame++);
        rhi::CommandBuffer cmd;
        const rhi::Extent2D sizes[2] = {{kW, kH}, {128, 96}};
        for (u32 v = 0; v < 2; ++v) {
            Scene3D scene(h.f.renderer.meshes(), mem::heap());
            Camera3D cam = orbit_camera(h.f.look_at, 0.0f, math::radians(h.f.pitch_deg), 7.0f);
            scene.begin(cam, sizes[v]);
            MeshInstance ground;
            ground.mesh = 2;
            scene.add(ground);
            MeshInstance box;
            box.mesh = 1;
            scene.add(box);
            caustics_scene(scene, &w, c, 0.0f);
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
    CHECK_FALSE(with_stats[0].caustics);
    CHECK(with_stats[1].caustics);
    for (const Forward3DStats& s : without_stats) CHECK_FALSE(s.caustics);
    int darkest = 0;
    usize lit = 0;
    for (u32 y = 24; y < 96; ++y)
        for (u32 x = 0; x < 128; ++x) {
            const usize p = usize(y) * kW + x;
            const int d = luma8(with, p) - luma8(without, p);
            darkest = std::max(darkest, -d);
            lit += d >= 2 ? 1 : 0;
        }
    MESSAGE("the window view after the render-texture view: " << lit << " lit pixels");
    CHECK(darkest == 0);
    CHECK(lit > 300);
}

TEST_CASE("caustics: the exp² fog takes its share of the light") {
    CausticsFixture h;
    const WaterSurface w = caustics_water();
    const Caustics on = caustics_setting();
    const auto fog = [](Scene3D& s) {
        Fog f = s.fog();
        f.density = 0.12f;  // the floor 6..12 m away keeps exp(-0.5..-2) of its own colour
        s.set_fog(f);
    };
    const Light clear = light(h.draw(&w, &on).px, h.draw(&w, nullptr).px);
    const Shot fog_on = h.draw(&w, &on, 0.0f, kCausticsToward, fog);
    const Light fogged = light(fog_on.px, h.draw(&w, nullptr, 0.0f, kCausticsToward, fog).px);
    MESSAGE("luma added without fog " << clear.sum << ", with fog " << fogged.sum);
    CHECK(fog_on.stats.caustics);
    CHECK(fogged.darkest == 0);
    CHECK(fogged.sum > 0);
    CHECK(f64(fogged.sum) < 0.6 * f64(clear.sum));
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
    u32 hh = sign | (u32(e) << 10) | (m >> 13);
    m &= 0x1fffu;
    if (m > 0x1000u || (m == 0x1000u && (hh & 1u))) ++hh;
    return u16(hh);
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
    d.debug_name = "test.caustics.input";
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

// CausticsU as the shader declares it (forward3d.cpp's mirror is private to the renderer).
struct CausticsBlock {
    f32 proj[4], right[4], up[4], back[4], sun[4], color[4], plane[4], misc[4];
};
static_assert(sizeof(CausticsBlock) == shaders3d::kCausticsUBytes, "CausticsBlock mirrors CausticsU");

// kCausticsFS alone over a w x 1 prepass row (depth per texel) and a black source into a w x 1 RGBA32F target: returns the
// red channel per texel (with c_color.r 1: the term). One row, so v_uv.y is 0.5 whatever the backend's row order.
Array<f32> run_pass(Fixture3D& f, const Array<f32>& depth, const CausticsBlock& u) {
    const u32 w = u32(depth.size());
    const rhi::TextureHandle gbuf = row_texture(f, w, [&](u32 x) { return math::vec4(0.0f, 0.0f, 1.0f, depth[x]); });
    const rhi::TextureHandle src = row_texture(f, w, [](u32) { return math::vec4(0.0f, 0.5f, 0.5f, 1.0f); });
    const rhi::PipelineHandle pipe = fullscreen_pipe(f, shaders3d::kCausticsFS, "caustics.frag");
    const rhi::PipelineHandle enc = fullscreen_pipe(f, kEncodeFS, "test.caustics.encode");
    rhi::TextureDesc td;
    td.size = {w, 1};
    td.format = rhi::TextureFormat::RGBA32F;
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.caustics.out";
    const rhi::TextureHandle rt = f.device->create_texture(td);
    td.format = rhi::TextureFormat::RGBA8_UNorm;
    td.debug_name = "test.caustics.encode";
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
    pass(rt, "test.caustics");
    cmd.bind_pipeline(pipe);
    cmd.set_uniforms(rhi::bank::kPass, rhi::as_bytes(u));
    cmd.bind_texture(rhi::bank::kAlbedo, src, point);
    cmd.bind_texture(rhi::bank::kNormal, gbuf, point);
    cmd.draw_non_indexed(3);
    cmd.end_pass();
    pass(bytes, "test.caustics.encode");
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

TEST_CASE("caustics: the shader agrees with the CPU twin on a grid of points") {
    // Its own string, under the 65536-byte literal limit (kMeshFS and kTerrainFS are not touched).
    MESSAGE("kCausticsFS: " << std::strlen(shaders3d::kCausticsFS) << " bytes");
    CHECK(std::strlen(shaders3d::kCausticsFS) <= 65536u);
    Fixture3D f;
    constexpr u32 kN = 128;
    Array<f32> depth;
    depth.resize(kN);
    // 0 = sky; 1.5..33.25 m in quarter metres (exact in half), and one overflowed depth (6e4 and more: sky).
    for (u32 x = 0; x < kN; ++x)
        depth[x] = x % 32 == 0 ? 0.0f : (x == 77 ? 65000.0f : f32(6 + x) * 0.25f);
    struct Case {
        f32 yaw, pitch;  // the camera's yaw and pitch (radians; pitch > 0 looks down)
        vec3 toward;     // toward the sun (normalized as the renderer does)
        f32 plane_h;     // the water level minus the camera's y
        math::vec2 centre;
        f32 half_extent, scale;
        math::vec2 offset;
        f32 phase;
        u32 octaves;
        f32 absorption, max_depth;
        f32 fog = 0.0f;
    };
    const Case cases[] = {
        {0.0f, 0.6f, {0.3f, 0.9f, 0.3f}, -3.0f, {1.5f, -2.0f}, 500.0f, 1.5f, {5.0f, 61.0f}, 0.0f, 3, 0.15f, 30.0f},
        {1.1f, 0.9f, {-0.5f, 0.4f, 0.2f}, -1.0f, {0.0f, 0.0f}, 500.0f, 2.0f, {12.25f, 63.5f}, 0.375f, 2, 0.05f, 60.0f},
        {-2.4f, 0.3f, {0.1f, 0.15f, -0.9f}, -0.5f, {0.0f, 0.0f}, 20.0f, 0.5f, {31.0f, 7.75f}, 0.9f, 1, 0.3f, 8.0f},
        {3.0f, 0.8f, {0.0f, 1.0f, 0.0f}, 4.0f, {-3.0f, 2.0f}, 500.0f, 4.0f, {0.5f, 0.25f}, 0.6f, 3, 0.1f, 40.0f},  // under
        {0.7f, -0.4f, {0.6f, 0.05f, 0.8f}, 12.0f, {0.0f, 0.0f}, 500.0f, 1.0f, {50.0f, 1.0f}, 0.1f, 3, 0.0f, 200.0f},  // up
        {0.2f, 0.7f, {0.3f, 0.7f, -0.4f}, -2.0f, {0.0f, 0.0f}, 500.0f, 1.5f, {3.5f, 9.0f}, 0.25f, 3, 0.15f, 30.0f, 0.03f},
    };
    const f32 fov = math::radians(45.0f), aspect = f32(kN);
    usize compared = 0, hits = 0, lit = 0, nonzero = 0, close = 0;
    f32 worst = 0.0f;
    for (const Case& c : cases) {
        const f32 cy = std::cos(c.yaw), sy = std::sin(c.yaw), cp = std::cos(c.pitch), sp = std::sin(c.pitch);
        const vec3 fwd{-sy * cp, -sp, -cy * cp};
        const vec3 right = math::normalize(math::cross(fwd, vec3{0.0f, 1.0f, 0.0f}));
        const vec3 up = math::cross(right, fwd);
        const vec3 back = -fwd;
        const f32 p11 = 1.0f / std::tan(fov * 0.5f), p00 = p11 / aspect * 80.0f;  // a wide row: about 70 degrees across
        const f32 p20 = 0.0125f, p21 = -0.02f;                                     // an off-centre (jittered) projection
        const vec3 sun = math::normalize(c.toward);
        CausticsBlock u{};
        const f32 blk[8][4] = {{p00, p11, p20, p21},
                               {right.x, right.y, right.z, 1.0f / c.scale},
                               {up.x, up.y, up.z, c.plane_h},
                               {back.x, back.y, back.z, c.half_extent},
                               {sun.x, sun.y, sun.z, c.absorption},
                               {1.0f, 0.0f, 0.0f, c.max_depth},
                               {c.offset.x, c.offset.y, c.centre.x, c.centre.y},
                               {c.phase, f32(c.octaves), c.fog, 0.0f}};
        std::memcpy(&u, blk, sizeof u);
        const Array<f32> gpu = run_pass(f, depth, u);
        CausticsParams cp2;
        cp2.toward_sun = sun;
        cp2.plane_h = c.plane_h;
        cp2.centre = c.centre;
        cp2.half_extent = c.half_extent;
        cp2.inv_scale = 1.0f / c.scale;
        cp2.offset = c.offset;
        cp2.phase = c.phase;
        cp2.octaves = c.octaves;
        cp2.absorption = c.absorption;
        cp2.max_depth = c.max_depth;
        cp2.fog_density = c.fog;
        for (u32 x = 0; x < kN; ++x) {
            f32 want = 0.0f;
            const f32 z = depth[x];
            if (z > 0.0f && z < 6.0e4f) {
                const f32 ndc_x = (f32(x) + 0.5f) / f32(kN) * 2.0f - 1.0f;
                const vec3 v{(ndc_x + p20) * z / p00, (0.0f + p21) * z / p11, -z};
                const vec3 p{right.x * v.x + up.x * v.y + back.x * v.z, right.y * v.x + up.y * v.y + back.y * v.z,
                             right.z * v.x + up.z * v.y + back.z * v.z};
                math::vec2 xz;
                f32 d = 0.0f;
                if (caustics_plane_point(p, cp2, xz, d)) ++hits;
                want = caustics_term(p, cp2);
                lit += want > 0.05f ? 1 : 0;
                nonzero += want > 0.0f ? 1 : 0;
            }
            const f32 err = std::fabs(gpu[x] - want);
            worst = std::max(worst, err);
            close += err <= 1e-4f ? 1 : 0;
            if (!(z > 0.0f && z < 6.0e4f)) CHECK(gpu[x] == 0.0f);  // sky and overflow: exactly the source
            CHECK_MESSAGE(err <= 2e-3f, "case yaw " << c.yaw << " texel " << x << ": gpu " << gpu[x] << " cpu " << want);
            ++compared;
        }
    }
    MESSAGE("shader vs CPU twin: " << compared << " points (" << hits << " under the plane, " << nonzero << " with light, "
                                    << lit << " over 0.05), worst |diff| " << worst << ", " << close << " within 1e-4");
    CHECK(hits > compared / 3);
    CHECK(nonzero > compared / 4);
    CHECK(lit > 20);
    CHECK(close > compared * 9 / 10);
}

// The pass's GPU cost per tier, opt in (MCE_CAUSTICS_COST=1; not part of the regular run): the bloom bucket of the GPU
// timestamps (kTsPostFx..kTsBloom: the cloud shadow, the caustics, the shafts, the flare, the underwater look, the lens
// rain and the haze; only the caustics are on here) at 1280 x 720, the mean of 16 frames after 4 warm-up frames, with and
// without the pass. Prints, asserts nothing about the numbers.
TEST_CASE("caustics: GPU cost per tier at 1280 x 720 (opt in)") {
    if (!std::getenv("MCE_CAUSTICS_COST")) return;
    CausticsFixture h;
    if (!h.f.device->caps().timestamp_queries) return;
    const rhi::Extent2D size{1280, 720};
    rhi::TextureDesc td;
    td.size = size;
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.caustics.cost";
    const rhi::TextureHandle target = h.f.device->create_texture(td);
    const WaterSurface w = caustics_water();
    const Caustics on = caustics_setting();
    auto mean_ms = [&](const Caustics* c) {
        f64 sum = 0.0;
        u32 n = 0;
        for (u32 i = 0; i < 20; ++i) {
            h.f.device->begin_frame(h.f.frame++);
            rhi::CommandBuffer cmd;
            Scene3D scene(h.f.renderer.meshes(), mem::heap());
            Camera3D cam = orbit_camera(h.f.look_at, 0.0f, math::radians(h.f.pitch_deg), 7.0f);
            scene.begin(cam, size);
            MeshInstance ground;
            ground.mesh = 2;
            scene.add(ground);
            caustics_scene(scene, &w, c, 0.0f);
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
        MESSAGE("caustics cost, " << (k == 0 ? 2 : 3) << " octaves (Medium 2, High 3), 1280 x 720: bloom bucket " << without
                                  << " ms without, " << with << " ms with, pass ~" << with - without << " ms");
    }
    h.f.device->destroy_texture(target);
    CHECK(h.f.device->validation_error_count() == 0);
}
