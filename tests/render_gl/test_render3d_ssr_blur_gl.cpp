// M-REFLECT2 48c against a real driver (GL 4.5 on llvmpipe and, as test_render_sdlgpu, the SDL GPU backend): the
// roughness-driven gather of the SSR lookup in kMeshFS. The probe room (a metal floor before a red emissive wall, a dim
// IBL sky, no sun) with the floor's roughness swapped: at or below kSsrBlurMinRough (the ssr_room golden's 0.1 among
// them) the frame is byte-identical with r.ssr.blur 1 and 0; over a ladder of roughnesses the reflected wall's top edge
// widens monotonically (measured in pixels along a screen column), by about what ssr_blur_radius() predicts; the
// reflection's mean red does not drop (a missed tap lowers the confidence, not the colour); a floor whose weight is 0
// (roughness above max_roughness) is untouched; two runs agree; MSAA 1 and 4 both widen; Forward3DStats::ssr_blur
// says when the gather was on. Each shot is a fresh fixture rendering two frames (the second reflects the first), as
// the golden does. 160 x 120 frames. M-REFLECT2 48d: a noisy normal map widens the gather (the bump term) and keeps the
// weight; a flat map and a flat terrain reflect as no map. 48e: with TAA's velocity, a moving cube's reflection follows
// it (r.ssr.motion); a still scene and a frame without velocity are unchanged.

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
    f32 max_roughness = 1.0f;  // ScreenSpaceReflections::max_roughness (the default 0.6 in the golden-like cases)
    u32 msaa = 4;
    int map = -1;          // M-REFLECT2 48d: -1 the mesh's own material, 0 an override without a normal map, 1 a flat
                           // (128, 128, 255) normal map, 2 a noisy one
    bool terrain = false;  // the floor drawn as a one-layer untextured terrain (kTerrainFS) at `rough`
};

struct Shot {
    Array<u8> px;
    Forward3DStats st;
    View3D view;
};

// A 64 x 64 normal map: flat (128, 128, 255), or each texel tilted by up to about 17 degrees in a fixed pseudo-random
// direction (an LCG, the same every run).
u32 normal_map(Fixture3D& f, bool noisy) {
    constexpr u32 N = 64;
    Array<u8> texels;
    u32 seed = 12345u;
    const auto rnd = [&seed] {  // -1..1
        seed = seed * 1664525u + 1013904223u;
        return f32(seed >> 8) / f32(1u << 23) - 1.0f;
    };
    for (u32 i = 0; i < N * N; ++i) {
        f32 nx = 0.0f, ny = 0.0f;
        if (noisy) {
            nx = 0.3f * rnd();
            ny = 0.3f * rnd();
        }
        const f32 nz = std::sqrt(1.0f - nx * nx - ny * ny);
        texels.push_back(noisy ? u8(std::lround((nx * 0.5f + 0.5f) * 255.0f)) : 128);
        texels.push_back(noisy ? u8(std::lround((ny * 0.5f + 0.5f) * 255.0f)) : 128);
        texels.push_back(noisy ? u8(std::lround((nz * 0.5f + 0.5f) * 255.0f)) : 255);
        texels.push_back(255);
    }
    const TextureMipUpload mip{N, N, std::as_bytes(texels.span())};
    TextureUpload up;
    up.mips = Span<const TextureMipUpload>(&mip, 1);
    const u32 key = f.renderer.textures().allocate_key();
    REQUIRE(f.renderer.textures().upload(key, 1, up).has_value());
    return key;
}

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
    fb.uvs = d.map >= 0;
    floor.add_box(fb);
    floor.submeshes.back().material.metallic = 1.0f;
    floor.submeshes.back().material.roughness = d.rough;
    REQUIRE(f.renderer.meshes().upload(9, 1, floor.build().span()).has_value());
    MaterialOverride mat;
    if (d.map >= 0) {
        mat.params.base_color[0] = mat.params.base_color[1] = mat.params.base_color[2] = mat.params.base_color[3] =
            1.0f;
        mat.params.metallic = 1.0f;
        mat.params.roughness = d.rough;
        mat.params.uv_scale_half = asset::MaterialChunk::pack_uv_scale(2.0f, 2.0f);
        if (d.map > 0) mat.normal_key = normal_map(f, d.map == 2);
    }
    TerrainShading ts;  // one untextured layer: the white splat stand-in weighs it 1, its normal is the geometric one
    ts.layer_count = 1;
    ts.size = {12.0f, 12.0f};
    ts.splat_resolution = 2;
    ts.layers[0].metallic = 1.0f;
    ts.layers[0].roughness = d.rough;
    const auto scene = [&](Scene3D& s) {
        no_sun(s);
        s.set_sky(constant_sky(0.05f, true));
        MeshInstance m;
        m.mesh = 9;
        if (d.map >= 0) m.material = &mat;
        if (d.terrain) m.terrain = &ts;
        s.add(m);
        m.material = nullptr;
        m.terrain = nullptr;
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
        d.max_roughness = 0.6f;  // the default, as the golden
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

// ---- M-REFLECT2 48d: a normal map widens the gather (the bump term), the blend weight keeps the material roughness

TEST_CASE("render3d_gl ssr bump: a flat normal map reflects as no map; a noisy one wider and no darker") {
    for (f32 rough : {0.2f, 0.3f}) {
        ShotDesc d;
        d.rough = rough;
        d.map = 0;
        const Shot none = shoot(d);
        d.map = 1;
        const Shot flat = shoot(d);
        d.map = 2;
        const Shot noisy = shoot(d);
        d.blur = false;
        const Shot noisy_sharp = shoot(d);
        const Profile pn = edge_profile(none), pf = edge_profile(flat), pb = edge_profile(noisy),
                      ps = edge_profile(noisy_sharp);
        MESSAGE("rough " << rough << ": width no map " << pn.width << ", flat map " << pf.width << ", noisy map "
                         << pb.width << " (sharp " << ps.width << "); mean red " << pn.mean << " / " << pf.mean << " / "
                         << pb.mean << " (sharp " << ps.mean << ")");
        INFO("rough " << rough);
        CHECK(noisy.st.ssr_blur);
        // A (128, 128, 255) map is not exactly flat (128/255 = 0.502: a 0.2 degree tilt, a bump term of about 5e-5 and
        // no visible radius); a few pixels at silhouettes, where the derivative tangent frame of perturb_normal breaks
        // down, tilt far and gather wider. Measured on llvmpipe: 2 pixels (rough 0.2) and 7 (0.3) of 19200 differ by
        // more than 3 levels, at most 9 and 32; with r.ssr.blur 0 the same frames differ at 0 and 4 pixels (the
        // shading).
        usize differ = 0;
        int diff = 0;
        for (usize i = 0; i < flat.px.size(); ++i) {
            const int q = std::abs(int(flat.px[i]) - int(none.px[i]));
            diff = std::max(diff, q);
            differ += q > 3;
        }
        MESSAGE("flat map against no map: " << differ << " channel values differ by more than 3, at most by " << diff);
        CHECK(differ <= 20);
        CHECK(std::abs(pf.width - pn.width) <= 1);
        CHECK(std::abs(pf.mean - pn.mean) <= pn.mean * 0.02);
        CHECK(pb.width >= pf.width + 2);  // the bump term widens the reflected edge
        CHECK(pb.width > ps.width);
        // The weight keeps the material roughness: the noisy floor still reflects about as much as the flat one.
        CHECK(pb.mean >= pf.mean * 0.9);
        CHECK(pb.mean <= pf.mean * 1.25);
        CHECK(pb.hi > pb.lo + 40);
    }
}

TEST_CASE("render3d_gl ssr bump: a noisy-mapped mirror below the threshold keeps the single tap") {
    ShotDesc d;
    d.rough = 0.05f;
    d.max_roughness = 0.6f;  // the default: the wall and the box (0.8) take no SSR, so only the floor could change
    d.map = 2;
    const Shot on = shoot(d);
    d.blur = false;
    const Shot off = shoot(d);
    CHECK(on.st.ssr_blur);
    CHECK(same(on.px, off.px));  // ssr_blur_radius is 0 at or below kSsrBlurMinRough whatever the bump
}

TEST_CASE("render3d_gl ssr bump: a terrain whose splat normal is the geometric one reflects as the mesh floor") {
    ShotDesc d;
    d.rough = 0.3f;
    const Shot mesh = shoot(d);
    d.terrain = true;
    const Shot terrain = shoot(d);
    const Profile pm = edge_profile(mesh), pt = edge_profile(terrain);
    MESSAGE("rough 0.3: width mesh " << pm.width << ", terrain " << pt.width << "; mean red " << pm.mean << " / "
                                     << pt.mean);
    CHECK(terrain.st.ssr_blur);
    CHECK(std::abs(pt.width - pm.width) <= 1);
    d.rough = 0.05f;
    d.max_roughness = 0.6f;  // the default
    const Shot t_on = shoot(d);
    d.blur = false;
    CHECK(same(t_on.px, shoot(d).px));
}

// ---- M-REFLECT2 48e: the trace reads the velocity at its hit (r.ssr.motion) when TAA or motion blur made one

namespace {

struct MoveDesc {
    bool motion = true;  // r.ssr.motion
    bool taa = true;
    bool moving = true;  // the mover travels kStep along x per frame (else it stands at its last place)
    int frames = 6;
};
constexpr f32 kStep = 0.3f;                     // metres per frame
constexpr f32 kMoverZ = 0.8f, kMoverY = 0.55f;  // centre; a 0.4 m cube hovering 0.3 m over the mirror

struct MoveShot {
    Array<u8> px;
    Forward3DStats st;
    View3D view;
    f32 x = 0;  // the mover's centre in the last frame
};

// The probe room's mirror floor (roughness 0.05: one sharp tap), the red wall, and a green emissive cube moving along
// x in front of it; the last of `frames` frames read back.
MoveShot shoot_moving(const MoveDesc& d) {
    CvarScope motion("r.ssr.motion", d.motion ? "1" : "0");
    Fixture3D f(1);
    upload_probe_room(f);
    mesh_fixture::Builder floor;
    floor.add_node(-1);
    mesh_fixture::Box fb;
    fb.center = {0, 0.02f, 0};
    fb.half = {6, 0.03f, 6};
    floor.add_box(fb);
    floor.submeshes.back().material.metallic = 1.0f;
    floor.submeshes.back().material.roughness = 0.05f;
    REQUIRE(f.renderer.meshes().upload(9, 1, floor.build().span()).has_value());
    mesh_fixture::Builder cube;
    cube.add_node(-1);
    mesh_fixture::Box cb;
    cb.center = {0, 0, 0};
    cb.half = {0.2f, 0.2f, 0.2f};
    cb.color[0] = cb.color[1] = cb.color[2] = 0.0f;
    cube.add_box(cb);
    cube.submeshes.back().material.emissive[1] = 4.0f;
    REQUIRE(f.renderer.meshes().upload(10, 1, cube.build().span()).has_value());
    f32 x = -1.2f;
    math::mat4 prev, world;
    for (int i = 0; i < d.frames; ++i) {
        const f32 px = x;
        if (d.moving) x += kStep;
        prev = math::translation({px, kMoverY, kMoverZ});
        world = math::translation({x, kMoverY, kMoverZ});
        f.render(0.0f, false, [&](Scene3D& s) {
            no_sun(s);
            s.set_sky(constant_sky(0.05f, true));
            PostFx p;
            p.taa = d.taa;
            p.bloom = false;
            p.fxaa = false;
            s.set_post(p);
            MeshInstance m;
            m.mesh = 9;
            s.add(m);
            m.mesh = 8;
            s.add(m);
            m.mesh = 10;
            m.world = world;
            if (d.moving) m.prev_world = &prev;
            s.add(m);
            ScreenSpaceReflections ssr;
            ssr.enabled = true;
            s.set_reflections(ssr);
        });
    }
    CHECK(f.device->validation_error_count() == 0);
    return {f.pixels, f.renderer.forward_3d().last_stats(), f.view, x};
}

// The screen-x centroid of the green pixels (g above r and b by 40) in the rows of the cube's mirror image.
struct Centroid {
    f64 x = 0;
    u32 n = 0;
};
Centroid reflection_centroid(const MoveShot& s) {
    const f32 floor_y = 0.05f;
    const Px top = to_px(s.view, {s.x, 2.0f * floor_y - (kMoverY - 0.15f), kMoverZ});
    const Px bot = to_px(s.view, {s.x, 2.0f * floor_y - (kMoverY + 0.15f), kMoverZ});
    Centroid c;
    f64 sum = 0;
    for (int y = std::max(0, std::min(top.y, bot.y)); y <= std::min(int(kH) - 1, std::max(top.y, bot.y)); ++y)
        for (int x = 0; x < int(kW); ++x) {
            const u8* p = &s.px[(usize(y) * kW + usize(x)) * 4];
            if (int(p[1]) > int(p[0]) + 40 && int(p[1]) > int(p[2]) + 40) {
                sum += x + 0.5;
                ++c.n;
            }
        }
    c.x = c.n ? sum / c.n : -1.0;
    return c;
}

f64 truth_x(const MoveShot& s) {  // the mirror image's centre on screen
    const math::vec4 c = s.view.view_proj * math::vec4(vec3{s.x, 2.0f * 0.05f - kMoverY, kMoverZ}, 1.0f);
    return (c.x / c.w * 0.5 + 0.5) * kW;
}

int max_diff(const Array<u8>& a, const Array<u8>& b) {
    int m = 0;
    for (usize i = 0; i < a.size(); ++i) m = std::max(m, std::abs(int(a[i]) - int(b[i])));
    return m;
}

}  // namespace

TEST_CASE("render3d_gl ssr motion: a moving cube's reflection follows it with r.ssr.motion, trails without") {
    MoveDesc d;
    const MoveShot on = shoot_moving(d);
    d.motion = false;
    const MoveShot off = shoot_moving(d);
    const Centroid con = reflection_centroid(on), coff = reflection_centroid(off);
    const f64 truth = truth_x(on);
    MESSAGE("cube at x " << on.x << " m, " << kStep << " m per frame; reflection centroid: motion " << con.x << " px ("
                         << con.n << " px), camera only " << coff.x << " px (" << coff.n << " px), truth " << truth
                         << " px");
    CHECK(on.st.ssr_motion);
    CHECK_FALSE(off.st.ssr_motion);
    REQUIRE(con.n > 10);
    REQUIRE(coff.n > 0);
    const f64 err_on = std::abs(con.x - truth), err_off = std::abs(coff.x - truth);
    CHECK(err_on + 1.5 <= err_off);
    CHECK(con.n > coff.n);  // the leading part of the image is there too
    // Two runs agree.
    d.motion = true;
    CHECK(same(on.px, shoot_moving(d).px));
}

TEST_CASE("render3d_gl ssr motion: a still scene with TAA is the camera path within a bound; no velocity, no change") {
    MoveDesc d;
    d.moving = false;
    const MoveShot on = shoot_moving(d);
    d.motion = false;
    const MoveShot off = shoot_moving(d);
    const int diff = max_diff(on.px, off.px);
    MESSAGE("still scene, TAA: the largest per-channel difference with r.ssr.motion 1 and 0 is " << diff);
    CHECK(on.st.ssr_motion);
    CHECK(diff <= 2);  // the jitter correction: the velocity path lands where the camera path does
    // Without TAA, motion blur or depth of field no velocity exists: r.ssr.motion changes nothing.
    d.taa = false;
    d.moving = true;
    const MoveShot plain_off = shoot_moving(d);
    d.motion = true;
    const MoveShot plain_on = shoot_moving(d);
    CHECK_FALSE(plain_on.st.ssr_motion);
    CHECK(plain_on.st.ssr);
    CHECK(same(plain_on.px, plain_off.px));
}

TEST_CASE("render3d_gl ssr motion: the velocity pass moved into the SSR stage keeps the stage times ascending") {
    CvarScope motion("r.ssr.motion", "1");
    Fixture3D f(1);
    if (!f.device->caps().timestamp_queries) return;  // the driver has no timer queries
    upload_probe_room(f);
    for (int i = 0; i < 6; ++i)
        f.render(0.0f, false, [](Scene3D& s) {
            PostFx p;
            p.taa = true;
            s.set_post(p);
            MeshInstance m;
            m.mesh = 7;
            s.add(m);
            ScreenSpaceReflections ssr;
            ssr.enabled = true;
            s.set_reflections(ssr);
        });
    CHECK(f.renderer.forward_3d().last_stats().ssr_motion);
    const Forward3DGpuTimes t = f.renderer.forward_3d().gpu_times(f.device->last_gpu_timestamps());
    CHECK(t.ssao >= 0.0);
    CHECK(t.ssr >= 0.0);      // kTsSsao <= kTsSsr
    CHECK(t.contact >= 0.0);  // kTsSsr <= kTsContact
    CHECK(t.taa >= 0.0);
    CHECK(t.total >= t.ssr);
    CHECK(f.device->validation_error_count() == 0);
}
