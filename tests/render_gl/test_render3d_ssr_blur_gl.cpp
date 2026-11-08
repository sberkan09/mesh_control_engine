// M-REFLECT2 48c against a real driver (GL 4.5 on llvmpipe and, as test_render_sdlgpu, the SDL GPU backend): the
// roughness-driven gather of the SSR lookup in kMeshFS. The probe room (a metal floor before a red emissive wall, a dim
// IBL sky, no sun) with the floor's roughness swapped: at or below kSsrBlurMinRough (the ssr_room golden's 0.1 among
// them) the frame is byte-identical with r.ssr.blur 1 and 0; over a ladder of roughnesses the reflected wall's top edge
// widens monotonically (measured in pixels along a screen column), by about what ssr_blur_radius() predicts; the
// reflection's mean red does not drop (a missed tap lowers the confidence, not the colour); a floor whose weight is 0
// (roughness above max_roughness) is untouched; two runs agree; MSAA 1 and 4 both widen; Forward3DStats::ssr_blur
// says when the gather was on. Each shot is a fresh fixture rendering two frames (the second reflects the first), as
// the golden does. 160 x 120 frames.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "../render/mesh_fixture.h"
#include "fixture3d.h"
#include "mce/convar.h"
#include "mce/render/ssr_blur.h"

using namespace mce;
using namespace mce::render;
using math::vec3;
using namespace mce::render_test;

namespace {

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

struct ShotDesc {
    f32 rough = 0.4f;          // the floor's roughness (mesh 9)
    bool blur = true;          // r.ssr.blur
    bool ssr = true;           // ScreenSpaceReflections::enabled
    f32 max_roughness = 1.0f;  // ScreenSpaceReflections::max_roughness (the default 0.5 in the golden-like cases)
    u32 msaa = 4;
};

struct Shot {
    Array<u8> px;
    Forward3DStats st;
    View3D view;
};

// The probe room with the floor at `d.rough`, two frames, the second read back.
Shot shoot(const ShotDesc& d) {
    CvarScope blur("r.ssr.blur", d.blur ? "1" : "0");
    Fixture3D f(d.msaa);
    upload_probe_room(f);
    mesh_fixture::Builder floor;
    floor.add_node(-1);
    mesh_fixture::Box fb;
    fb.center = {0, 0.02f, 0};
    fb.half = {6, 0.03f, 6};
    floor.add_box(fb);
    floor.submeshes.back().material.metallic = 1.0f;
    floor.submeshes.back().material.roughness = d.rough;
    REQUIRE(f.renderer.meshes().upload(9, 1, floor.build().span()).has_value());
    const auto scene = [&d](Scene3D& s) {
        no_sun(s);
        s.set_sky(constant_sky(0.05f, true));
        MeshInstance m;
        m.mesh = 9;
        s.add(m);
        m.mesh = 8;
        s.add(m);
        ScreenSpaceReflections ssr;
        ssr.enabled = d.ssr;
        ssr.max_roughness = d.max_roughness;
        s.set_reflections(ssr);
    };
    for (int i = 0; i < 2; ++i) f.render(0.0f, false, scene);
    CHECK(f.device->validation_error_count() == 0);
    return {f.pixels, f.renderer.forward_3d().last_stats(), f.view};
}

bool same(const Array<u8>& a, const Array<u8>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

struct Px {
    int x, y;
};
Px to_px(const View3D& v, vec3 p) {
    const math::vec4 c = v.view_proj * math::vec4(p, 1.0f);
    return {int((c.x / c.w * 0.5f + 0.5f) * kW), int((0.5f - c.y / c.w * 0.5f) * kH)};
}

// The red channel down the screen column of floor x = 2 m (beside the box), from the floor at z = 0.3 m (the wall's
// mirror image) to z = 2.8 m (the mirror ray passes over the wall: the sky), averaged over 3 columns.
struct Profile {
    int lo = 255, hi = 0;
    int width = 0;  // rows strictly between 10 % and 90 % of the way from lo to hi: the edge's transition width
    f64 mean = 0;   // the mean red over the column segment
};
Profile edge_profile(const Shot& s) {
    const Px a = to_px(s.view, {2.0f, 0.05f, 0.3f}), b = to_px(s.view, {2.0f, 0.05f, 2.8f});
    const int y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    Profile p;
    int vals[kH] = {};
    f64 sum = 0;
    for (int y = y0; y <= y1; ++y) {
        int r = 0;
        for (int dx = -1; dx <= 1; ++dx) r += s.px[(usize(y) * kW + usize(a.x + dx)) * 4];
        vals[y] = r / 3;
        p.lo = std::min(p.lo, vals[y]);
        p.hi = std::max(p.hi, vals[y]);
        sum += vals[y];
    }
    p.mean = sum / f64(y1 - y0 + 1);
    const f64 t0 = p.lo + 0.1 * (p.hi - p.lo), t1 = p.lo + 0.9 * (p.hi - p.lo);
    for (int y = y0; y <= y1; ++y) p.width += vals[y] > t0 && vals[y] < t1;
    return p;
}

}  // namespace

TEST_CASE("render3d_gl ssr blur: a mirror at or below the threshold keeps the single tap, bit for bit") {
    for (f32 rough : {0.05f, kSsrBlurMinRough}) {  // 0.1 is the ssr_room golden's floor
        ShotDesc d;
        d.rough = rough;
        d.max_roughness = 0.5f;  // the default, as the golden
        const Shot on = shoot(d);
        d.blur = false;
        const Shot off = shoot(d);
        INFO("rough " << rough);
        CHECK(on.st.ssr_history);
        CHECK(on.st.ssr_blur);  // the gather is on for the frame; no pixel of this floor takes it
        CHECK_FALSE(off.st.ssr_blur);
        CHECK(same(on.px, off.px));
        CHECK(edge_profile(on).hi > edge_profile(on).lo + 60);  // the wall's reflection is there to blur
    }
}

TEST_CASE("render3d_gl ssr blur: the reflected edge widens over a roughness ladder, no darker than the sharp one") {
    const f32 ladder[] = {0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
    int prev = -1;
    int widths[5] = {}, sharp_widths[5] = {};
    for (int i = 0; i < 5; ++i) {
        ShotDesc d;
        d.rough = ladder[i];
        const Shot on = shoot(d);
        d.blur = false;
        const Shot off = shoot(d);
        const Profile pb = edge_profile(on), ps = edge_profile(off);
        widths[i] = pb.width;
        sharp_widths[i] = ps.width;
        MESSAGE("rough " << ladder[i] << " radius " << ssr_blur_radius(ladder[i], 0.0f) << " texels: width " << pb.width
                         << " px (sharp " << ps.width << "), red " << pb.lo << ".." << pb.hi << " mean " << pb.mean
                         << " (sharp " << ps.lo << ".." << ps.hi << " mean " << ps.mean << ")");
        CHECK(on.st.ssr_blur);
        CHECK_FALSE(same(on.px, off.px));
        CHECK(pb.width >= prev);  // monotone over the ladder
        prev = pb.width;
        CHECK(pb.width >= ps.width);
        // The miss-tap rule: the blurred reflection is not darker on average than the sharp one (a bound of 4 %); it
        // is somewhat brighter (the blur is in HDR, before the tonemap's shoulder): measured +1 % at 0.2 to +21 % at
        // 0.6 on llvmpipe, bounded at +30 %.
        CHECK(pb.mean >= ps.mean * 0.96);
        CHECK(pb.mean <= ps.mean * 1.3);
    }
    CHECK(widths[2] > widths[0]);  // 0.4 wider than 0.2
    CHECK(widths[4] >= widths[0] + 4);
    // The CPU twin against the rendered width: a disc of radius R half-size texels spreads the edge over about 2R..4R
    // full-size rows; the extra width over the sharp edge stays inside [R, 4R + 2] rows at every rung.
    for (int i = 0; i < 5; ++i) {
        const f32 r = ssr_blur_radius(ladder[i], 0.0f);
        const int extra = widths[i] - sharp_widths[i];
        INFO("rough " << ladder[i] << " R " << r << " extra " << extra);
        CHECK(f32(extra) >= std::floor(r) - 1.0f);
        CHECK(f32(extra) <= 4.0f * r + 2.0f);
    }
}

TEST_CASE("render3d_gl ssr blur: a floor above max_roughness is untouched; r.ssr.blur 0 and SSR off as before") {
    ShotDesc d;
    d.rough = 0.8f;
    d.max_roughness = 0.5f;  // the weight is 0: the gather is skipped, the frame equals SSR off
    const Shot on = shoot(d);
    CHECK(on.st.ssr);
    CHECK(on.st.ssr_blur);
    d.ssr = false;
    const Shot off = shoot(d);
    CHECK_FALSE(off.st.ssr);
    CHECK_FALSE(off.st.ssr_blur);
    CHECK(same(on.px, off.px));
    d.ssr = true;
    d.blur = false;
    const Shot noblur = shoot(d);
    CHECK_FALSE(noblur.st.ssr_blur);
    CHECK(same(on.px, noblur.px));
    // A max_roughness at or below the threshold: no pixel could gather, the flag stays off.
    d.blur = true;
    d.rough = 0.05f;
    d.max_roughness = kSsrBlurMinRough;
    CHECK_FALSE(shoot(d).st.ssr_blur);
}

TEST_CASE("render3d_gl ssr blur: two runs agree; MSAA 1 widens too") {
    ShotDesc d;
    d.rough = 0.5f;
    const Shot a = shoot(d), b = shoot(d);
    CHECK(same(a.px, b.px));
    d.msaa = 1;
    const Shot m1 = shoot(d);
    d.blur = false;
    const Shot m1_sharp = shoot(d);
    d.rough = 0.2f;
    d.blur = true;
    const Shot m1_low = shoot(d);
    const Profile pw = edge_profile(m1), ps = edge_profile(m1_sharp), pl = edge_profile(m1_low);
    MESSAGE("msaa 1: width 0.5 " << pw.width << " (sharp " << ps.width << "), 0.2 " << pl.width);
    CHECK(pw.width > ps.width);
    CHECK(pw.width > pl.width);
    CHECK(pw.mean >= ps.mean * 0.96);
}
