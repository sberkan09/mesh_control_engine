// The 3D frame against a real GL 4.5 driver (CI: Xvfb + Mesa llvmpipe): a lit box on a ground
// slab rendered through cascaded shadows -> Forward+ PBR HDR (MSAA) + sky -> resolve -> bloom ->
// tonemap -> FXAA into an offscreen target, read back and probed: lit faces, the box's shadow on
// the ground, fog, MSAA on/off agreement, metal vs dielectric, clustered point lights, the sky,
// the post toggles, GPU timestamps, the GPU light assignment against the CPU one, the RHI additions (multisample resolve, layered depth targets,
// depth-compare array sampling, storage data, snorm/unorm16 vertex streams) and zero GL
// validation errors.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <utility>

#include "../render/mesh_fixture.h"
#include "fixture3d.h"
#include "mce/convar.h"
#include "mce/core.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/quality.h"
#include "mce/render/renderer.h"
#include "mce/rhi/default_device.h"

using namespace mce;
using namespace mce::render;
using math::vec3;
using namespace mce::render_test;

TEST_CASE("render3d_gl: lit box, its shadow on the ground, zero validation errors") {
    Fixture3D f;
    f.render();
    const Rgb top = f.at_world({0, 1.0f, 0});        // the box's top face, facing the sun
    const Rgb shadow = f.at_world({-0.8f, 0, -0.3f});  // ground behind the box, in its shadow
    const Rgb lit = f.at_world({2.0f, 0, 2.0f});        // open ground
    INFO("top " << top.r << "," << top.g << "," << top.b << " shadow " << shadow.luma() << " lit " << lit.luma());
    CHECK(top.r > top.g + 40);           // the red material survives lighting + tonemap
    CHECK(lit.luma() > shadow.luma() + 30);  // the shadow pass darkens the ground
    const Forward3DStats& st = f.renderer.forward_3d().last_stats();
    CHECK(st.cascades == 3);
    CHECK(st.shadow_draws >= 3);  // the ground in every cascade, the box in the near ones
    CHECK(st.shadow_draws <= 6);
    CHECK(st.main_draws == 2);
    CHECK(st.bloom_levels > 0);
    CHECK(st.fxaa);
    // A pass per cascade, forward, bloom down + up chains, tonemap, FXAA.
    CHECK(f.gpu.passes == st.cascades + 1 + (2 * st.bloom_levels - 1) + 1 + 1);
    CHECK(f.device->validation_error_count() == 0);

    // Shadows off: the same ground point is lit, and no shadow passes run.
    f.render(0.0f, false);
    CHECK(f.at_world({-0.8f, 0, -0.3f}).luma() > shadow.luma() + 30);
    CHECK(f.renderer.forward_3d().last_stats().cascades == 0);
    CHECK(f.gpu.passes == 1 + (2 * f.renderer.forward_3d().last_stats().bloom_levels - 1) + 1 + 1);
}

TEST_CASE("render3d_gl: post toggles: bloom and FXAA off leave forward + tonemap") {
    Fixture3D f;
    const auto no_post = [](Scene3D& s) {
        PostFx p;
        p.bloom = false;
        p.fxaa = false;
        s.set_post(p);
    };
    f.render(0.0f, true, no_post);
    const Forward3DStats& st = f.renderer.forward_3d().last_stats();
    CHECK(st.bloom_levels == 0);
    CHECK_FALSE(st.fxaa);
    CHECK(f.gpu.passes == st.cascades + 2);
    const Rgb plain = f.at_world({2.0f, 0, 2.0f});
    // Bloom adds a little light, FXAA leaves flat areas alone: the open ground barely changes.
    f.render();
    const Rgb post = f.at_world({2.0f, 0, 2.0f});
    CHECK(std::abs(plain.luma() - post.luma()) <= 12);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the sky dome fills the background above the horizon") {
    Fixture3D f;
    f.pitch_deg = 5.0f;  // looking nearly level: the top rows are above the horizon
    f.render();
    const usize i = (usize(2) * kW + kW / 2) * 4;
    const Rgb top{f.pixels[i], f.pixels[i + 1], f.pixels[i + 2]};
    INFO("sky " << top.r << "," << top.g << "," << top.b);
    CHECK(top.b > top.r + 10);
    CHECK(top.luma() > 120);
    // Sky off: the fog color clear (also pale blue), equal across the row.
    f.render(0.0f, true, [](Scene3D& s) {
        Sky sky;
        sky.enabled = false;
        s.set_sky(sky);
    });
    const Rgb a{f.pixels[i], f.pixels[i + 1], f.pixels[i + 2]};
    const usize j = (usize(2) * kW + 4) * 4;
    CHECK(std::abs(int(f.pixels[j]) - a.r) <= 2);
    CHECK(std::abs(int(f.pixels[j + 2]) - a.b) <= 2);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a metal box has no diffuse term; a rough dielectric does") {
    Fixture3D f;
    const auto upload = [&](u32 key, f32 metallic) {
        mesh_fixture::Builder mb;
        mb.add_node(-1);
        mesh_fixture::Box b;
        b.center = {0, 0.5f, 0};
        b.half = {0.5f, 0.5f, 0.5f};
        b.color[0] = 0.9f;
        b.color[1] = 0.3f;
        b.color[2] = 0.2f;
        mb.add_box(b);
        mb.submeshes.back().material.metallic = metallic;
        mb.submeshes.back().material.roughness = 0.9f;
        REQUIRE(f.renderer.meshes().upload(key, 1, mb.build().span()).has_value());
    };
    upload(5, 0.0f);
    upload(6, 1.0f);
    const auto with = [](u32 key) {
        return [key](Scene3D& s) {
            MeshInstance m;
            m.mesh = key;
            m.world = math::translation({1.6f, 0, 0.8f});
            s.add(m);
        };
    };
    f.render(0.0f, true, with(5));
    const Rgb dielectric = f.at_world({1.6f, 1.0f, 0.8f});
    f.render(0.0f, true, with(6));
    const Rgb metal = f.at_world({1.6f, 1.0f, 0.8f});
    INFO("dielectric " << dielectric.luma() << " metal " << metal.luma());
    CHECK(dielectric.r > dielectric.g + 40);
    CHECK(metal.luma() + 20 < dielectric.luma());  // rough metal off the specular peak: dark
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: clustered point and spot lights light only their neighbourhood") {
    Fixture3D f;
    // Night: no sun, no ambient, no sky reflections; 256 dim lights far off to the side plus one
    // red light just above the open ground at (2, 0, 2).
    const auto night = [](Scene3D& s) {
        DirectionalLight sun;
        sun.intensity = 0.0f;
        sun.ambient_sky = sun.ambient_ground = {0, 0, 0};
        sun.shadows = false;
        s.set_light(sun);
        Sky sky;
        sky.enabled = false;
        s.set_sky(sky);
        Fog fog;
        fog.color = {0, 0, 0};
        s.set_fog(fog);
    };
    f.render(0.0f, false, night);
    const Rgb dark = f.at_world({2.0f, 0, 2.0f});
    f.render(0.0f, false, [&](Scene3D& s) {
        night(s);
        for (u32 i = 0; i < 256; ++i) {
            PointLight l;
            const u32 col = i % 16, row = i / 16;
            l.position = {-5.5f + 0.04f * f32(col), 0.3f, -5.5f + 0.04f * f32(row)};
            l.radius = 0.4f;
            l.color = {0.2f, 0.2f, 1.0f};
            s.add_point_light(l);
        }
        PointLight red;
        red.position = {2.0f, 0.4f, 2.0f};
        red.radius = 1.5f;
        red.color = {1.0f, 0.1f, 0.05f};
        red.intensity = 0.15f;
        s.add_point_light(red);
    });
    const Rgb lit = f.at_world({2.0f, 0, 2.0f});
    const Rgb outside = f.at_world({-2.0f, 0, 2.0f});  // beyond the red light's radius
    INFO("dark " << dark.luma() << " lit " << lit.r << "," << lit.g << "," << lit.b << " outside " << outside.luma());
    CHECK(dark.luma() < 10);
    CHECK(lit.r > lit.g + 60);
    CHECK(outside.luma() < 10);
    const Forward3DStats& st = f.renderer.forward_3d().last_stats();
    CHECK(st.point_lights == 257);
    CHECK((st.gpu_clusters || st.light_indices > 0));  // the GPU path keeps its index count on the GPU

    // A spot light straight down with a 20° cone: the spot under it is lit, ground 45° off its
    // axis (well inside the radius) is not.
    f.render(0.0f, false, [&](Scene3D& s) {
        night(s);
        PointLight spot;
        spot.position = {2.0f, 1.0f, 2.0f};
        spot.radius = 4.0f;
        spot.color = {0.1f, 1.0f, 0.1f};
        spot.intensity = 0.4f;
        spot.direction = {0, -1, 0};
        spot.spot_inner = math::radians(10.0f);
        spot.spot_outer = math::radians(20.0f);
        s.add_point_light(spot);
    });
    const Rgb under = f.at_world({2.0f, 0, 2.0f});
    const Rgb off_axis = f.at_world({3.0f, 0, 2.0f});
    INFO("under " << under.r << "," << under.g << "," << under.b << " off axis " << off_axis.luma());
    CHECK(under.g > under.r + 60);
    CHECK(off_axis.luma() < 10);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: GPU timestamps time every stage") {
    Fixture3D f;
    if (!f.device->caps().timestamp_queries) return;  // the driver has no timer queries
    for (int i = 0; i < 6; ++i) f.render();  // timestamps are read back once their frame's fence passes
    const Forward3DGpuTimes t = f.renderer.forward_3d().gpu_times(f.device->last_gpu_timestamps());
    CHECK(t.frame_index != ~u64(0));
    CHECK(t.clusters >= 0.0);
    CHECK(t.ssao >= 0.0);
    CHECK(t.shadow >= 0.0);
    CHECK(t.gi >= 0.0);
    CHECK(t.main >= 0.0);
    CHECK(t.resolve >= 0.0);
    CHECK(t.bloom >= 0.0);
    CHECK(t.tonemap >= 0.0);
    CHECK(t.fxaa >= 0.0);
    CHECK(t.total >= t.main);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: dense fog converges on the fog color") {
    Fixture3D f;
    f.render(2.0f);
    const Rgb far_ground = f.at_world({3.0f, 0, -4.0f});
    // Fog (0.62, 0.72, 0.85) linear, through ACES + sRGB: a pale blue-grey with b > r.
    CHECK(far_ground.b > far_ground.r);
    CHECK(far_ground.luma() > 150);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: MSAA off renders the same picture away from edges") {
    Rgb a, b;
    {
        Fixture3D f(4);
        f.render();
        a = f.at_world({2.0f, 0, 2.0f});
    }
    {
        Fixture3D f(1);
        f.render();
        b = f.at_world({2.0f, 0, 2.0f});
        CHECK(f.device->validation_error_count() == 0);
    }
    CHECK(std::abs(a.r - b.r) <= 3);
    CHECK(std::abs(a.g - b.g) <= 3);
    CHECK(std::abs(a.b - b.b) <= 3);
}

TEST_CASE("render3d_gl: an animated node moves its submesh") {
    Fixture3D f;
    mesh_fixture::Builder spin;
    const u32 root = spin.add_node(-1);
    const u32 arm = spin.add_node(static_cast<i32>(root), {2, 0, 0});
    mesh_fixture::Box b;
    b.center = {0, 0.5f, 0};
    b.half = {0.3f, 0.5f, 0.3f};
    b.node = arm;
    b.color[0] = 0.1f;
    b.color[1] = 0.2f;
    b.color[2] = 0.9f;
    spin.add_box(b);
    spin.add_spin_clip(root, 4.0f);
    REQUIRE(f.renderer.meshes().upload(3, 1, spin.build().span()).has_value());
    const GpuMesh& m = *f.renderer.meshes().get(3);

    auto blue_at = [&](f32 t, vec3 probe) {
        math::mat4 pose[2];
        pose_nodes(m, 0, t, Span<math::mat4>(pose, 2));
        f.device->begin_frame(f.frame++);
        Scene3D scene(f.renderer.meshes(), mem::heap());
        scene.begin(orbit_camera({0, 0.5f, 0}, 0.0f, math::radians(60.0f), 9.0f), {kW, kH});
        MeshInstance ground;
        ground.mesh = 2;
        scene.add(ground);
        MeshInstance prop;
        prop.mesh = 3;
        prop.pose = Span<const math::mat4>(pose, 2);
        scene.add(prop);
        scene.build();
        f.view = scene.view();
        rhi::CommandBuffer cmd;
        f.renderer.record_3d(cmd, scene, f.target);
        f.device->submit(cmd);
        REQUIRE(f.device->read_pixels(f.target, rhi::Rect2D{0, 0, kW, kH}, f.pixels.span()).has_value());
        f.device->end_frame();
        const Rgb c = f.at_world(probe);
        return c.b > c.r + 40;
    };
    CHECK(blue_at(0.0f, {2, 1.0f, 0}));    // rest: the arm's box sits at +X
    CHECK_FALSE(blue_at(1.0f, {2, 1.0f, 0}));
    CHECK(blue_at(1.0f, {0, 1.0f, -2}));   // a quarter turn later it is at -Z
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a skinned bone bends with its joint; skinning off draws the bind pose") {
    Fixture3D f;
    // Joint A at the origin, joint B 1 m up; a green 1 m segment on each (FR-R3D-060), each its
    // own submesh and skin over {A, B}.
    mesh_fixture::Builder arm;
    const u32 root = arm.add_node(-1);
    const u32 a = arm.add_node(static_cast<i32>(root));
    const u32 b = arm.add_node(static_cast<i32>(a), {0, 1, 0});
    for (f32 y : {0.5f, 1.5f}) {
        mesh_fixture::Box seg;
        seg.center = {0, y, 0};
        seg.half = {0.2f, 0.5f, 0.2f};
        seg.node = root;
        seg.color[0] = 0.1f;
        seg.color[1] = 0.8f;
        seg.color[2] = 0.15f;
        arm.add_box(seg);
    }
    arm.skin_submesh(0, {a, b}, [](vec3) { return 0u; });
    arm.skin_submesh(1, {a, b}, [](vec3) { return 1u; });
    REQUIRE(f.renderer.meshes().upload(3, 1, arm.build().span()).has_value());
    const GpuMesh& m = *f.renderer.meshes().get(3);
    REQUIRE(m.skinned());

    // The rest pose, or joint B bent 90 degrees about +Z (the upper half lies along -X).
    auto green_at = [&](bool bent, bool skinning, vec3 probe) {
        math::mat4 pose[3];
        pose_nodes(m, ~0u, 0.0f, Span<math::mat4>(pose, 3));
        if (bent) pose[b] = pose[a] * math::trs({0, 1, 0}, math::quat_from_axis_angle({0, 0, 1}, math::kPi * 0.5f), {1, 1, 1});
        f.device->begin_frame(f.frame++);
        Scene3D scene(f.renderer.meshes(), mem::heap());
        scene.begin(orbit_camera({0, 1, 0}, 0.0f, math::radians(10.0f), 6.0f), {kW, kH});
        MeshInstance ground;
        ground.mesh = 2;
        scene.add(ground);
        MeshInstance inst;
        inst.mesh = 3;
        inst.pose = Span<const math::mat4>(pose, 3);
        inst.skinning = skinning;
        scene.add(inst);
        scene.build();
        CHECK(scene.joint_palette().size() == (skinning ? 4u : 0u));
        f.view = scene.view();
        rhi::CommandBuffer cmd;
        f.renderer.record_3d(cmd, scene, f.target);
        f.device->submit(cmd);
        REQUIRE(f.device->read_pixels(f.target, rhi::Rect2D{0, 0, kW, kH}, f.pixels.span()).has_value());
        f.device->end_frame();
        const Rgb c = f.at_world(probe);
        return c.g > c.r + 40 && c.g > c.b + 40;
    };
    const vec3 upper{0, 1.7f, 0.2f}, side{-0.6f, 1.0f, 0.2f}, lower{0, 0.4f, 0.2f};
    CHECK(green_at(false, true, upper));
    CHECK_FALSE(green_at(false, true, side));
    CHECK(green_at(true, true, side));  // bent: the upper half swung to -X
    CHECK_FALSE(green_at(true, true, upper));
    CHECK(green_at(true, true, lower));  // joint A did not move
    CHECK(f.renderer.forward_3d().last_stats().joints == 4);
    CHECK(green_at(true, false, upper));  // skeleton off: the bind pose, whatever the pose says
    CHECK_FALSE(green_at(true, false, side));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the GPU light assignment draws the CPU assignment's frame") {
    // Many overlapping point and spot lights across the view, some off-screen, one at zero
    // intensity and one behind the camera, at dusk so they dominate the frame.
    const auto lights = [](Scene3D& s) {
        DirectionalLight sun;
        sun.intensity = 0.2f;
        sun.shadows = true;
        sun.direction = {-0.5f, -1.0f, -0.3f};
        s.set_light(sun);
        u32 seed = 12345u;
        auto rnd = [&seed] {
            seed = seed * 1664525u + 1013904223u;
            return f32(seed >> 8) / f32(1u << 24);
        };
        for (u32 i = 0; i < 300; ++i) {
            PointLight l;
            l.position = {rnd() * 14.0f - 7.0f, rnd() * 2.0f, rnd() * 14.0f - 7.0f};
            l.radius = 0.3f + rnd() * 2.5f;
            l.color = {rnd(), rnd(), rnd()};
            l.intensity = 0.05f + rnd() * 0.3f;
            if (i % 7 == 0) {
                l.direction = {rnd() - 0.5f, -1.0f, rnd() - 0.5f};
                l.spot_outer = 0.3f + rnd() * 0.6f;
                l.spot_inner = l.spot_outer * 0.5f;
            }
            if (i == 11) l.intensity = 0.0f;
            if (i == 12) l.position = {0, 2, 20};  // behind the orbit camera
            s.add_point_light(l);
        }
    };
    Array<u8> cpu_px;
    {
        Fixture3D cpu(4, false);
        CHECK_FALSE(cpu.renderer.forward_3d().gpu_light_clusters());
        cpu.render(0.0f, true, lights);
        CHECK(cpu.cpu_assigned);
        CHECK_FALSE(cpu.renderer.forward_3d().last_stats().gpu_clusters);
        CHECK(cpu.gpu.dispatches == 0);
        cpu_px = cpu.pixels;
    }
    Fixture3D gpu(4, true);
    if (!gpu.device->caps().compute) return;
    CHECK(gpu.renderer.forward_3d().gpu_light_clusters());
    gpu.render(0.0f, true, lights);
    CHECK_FALSE(gpu.cpu_assigned);  // the frame never ran ClusterLights::assign()
    CHECK(gpu.renderer.forward_3d().last_stats().gpu_clusters);
    CHECK(gpu.gpu.dispatches == 1);
    REQUIRE(gpu.pixels.size() == cpu_px.size());
    usize differing = 0;
    for (usize i = 0; i < cpu_px.size(); ++i) differing += gpu.pixels[i] != cpu_px[i];
    CHECK(differing == 0);
    CHECK(gpu.device->validation_error_count() == 0);
}

// ---- image-based lighting (M-VISUAL 2) ----------------------------------------------------------

// A failed REQUIRE records but does not leave the test case here (no exceptions): bail out.
#define MCE_REQUIRE_OR_RETURN(...)                  \
    do {                                            \
        const bool mce_req_ok_ = (__VA_ARGS__);     \
        REQUIRE_MESSAGE(mce_req_ok_, #__VA_ARGS__); \
        if (!mce_req_ok_) return;                   \
    } while (0)

TEST_CASE("render3d_gl: image-based lighting off draws the frame without it") {
    Array<u8> without;
    {
        Fixture3D f(4, true, false);
        f.render();
        CHECK_FALSE(f.renderer.forward_3d().last_stats().ibl);
        without = f.pixels;
    }
    Fixture3D f;
    f.render();  // the resources exist, the scene does not ask for them
    CHECK_FALSE(f.renderer.forward_3d().last_stats().ibl);
    CHECK(f.renderer.forward_3d().ibl_env().is_null());
    usize differing = 0;
    for (usize i = 0; i < without.size(); ++i) differing += f.pixels[i] != without[i];
    CHECK(differing == 0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: IBL bakes the sky once per change: SH9, BRDF LUT and pre-filtered layers") {
    Fixture3D f;
    f.render(0.0f, true, [](Scene3D& s) { s.set_sky(constant_sky(0.2f, true)); });
    const Forward3D& fw = f.renderer.forward_3d();
    CHECK(fw.last_stats().ibl);
    CHECK(fw.last_stats().ibl_baked);
    MCE_REQUIRE_OR_RETURN(!fw.ibl_sh().is_null());

    // A constant sky c: SH coefficient 0 = c · 0.282095 · 4pi (= 0.709 for c = 0.2), the rest 0.
    u8 sh[9 * 4] = {};
    MCE_REQUIRE_OR_RETURN(f.device->read_pixels(fw.ibl_sh(), rhi::Rect2D{0, 0, 9, 1}, Span<u8>(sh, sizeof(sh))).has_value());
    INFO("sh0 " << int(sh[0]) << " sh1 " << int(sh[4]) << " sh6 " << int(sh[24]));
    CHECK(std::abs(int(sh[0]) - 181) <= 3);
    for (u32 k = 1; k < 9; ++k) CHECK(sh[k * 4] <= 3);

    // BRDF LUT (top row first = roughness 1): smooth and head-on reflects all of F0; rougher less.
    constexpr u32 n = Forward3D::kIblLutSize;
    Array<u8> lut;
    lut.resize(usize(n) * n * 4);
    MCE_REQUIRE_OR_RETURN(f.device->read_pixels(fw.ibl_brdf_lut(), rhi::Rect2D{0, 0, n, n}, lut.span()).has_value());
    const auto ab = [&](u32 nov_x, u32 rough_y) {  // rough_y 0 = smooth
        const usize i = (usize(n - 1 - rough_y) * n + nov_x) * 4;
        return std::pair<int, int>{lut[i], lut[i + 1]};
    };
    CHECK(ab(n - 1, 0).first >= 245);
    CHECK(ab(n - 1, 0).second <= 6);
    CHECK(ab(n - 1, n - 1).first < ab(n - 1, 0).first - 40);
    CHECK(ab(n - 1, n - 1).first + ab(n - 1, n - 1).second <= 255);
    CHECK(ab(2, 0).second > ab(n - 1, 0).second);  // grazing: Fresnel brightens F0's bias

    f.render(0.0f, true, [](Scene3D& s) { s.set_sky(constant_sky(0.2f, true)); });
    CHECK_FALSE(fw.last_stats().ibl_baked);  // same sky: nothing re-baked
    f.render(0.0f, true, [](Scene3D& s) { s.set_sky(constant_sky(0.3f, true)); });
    CHECK(fw.last_stats().ibl_baked);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: under a constant sky IBL matches the hemisphere ambient of the same color") {
    Fixture3D f;
    const auto scene = [](bool ibl) {
        return [ibl](Scene3D& s) {
            no_sun(s);
            DirectionalLight sun = s.light();
            sun.ambient_sky = sun.ambient_ground = {0.5f, 0.5f, 0.5f};
            s.set_light(sun);
            s.set_sky(constant_sky(0.5f, ibl));
        };
    };
    f.render(0.0f, false, scene(false));
    const Rgb box_off = f.at_world({0, 1.0f, 0}), ground_off = f.at_world({2.0f, 0, 2.0f});
    f.render(0.0f, false, scene(true));
    const Rgb box_on = f.at_world({0, 1.0f, 0}), ground_on = f.at_world({2.0f, 0, 2.0f});
    INFO("box " << box_off.luma() << " -> " << box_on.luma() << ", ground " << ground_off.luma() << " -> " << ground_on.luma());
    CHECK(std::abs(box_off.luma() - box_on.luma()) <= 8);
    CHECK(std::abs(ground_off.luma() - ground_on.luma()) <= 8);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: IBL takes the ambient's color from where the sky is") {
    Fixture3D f;
    f.pitch_deg = 50.0f;
    // Blue overhead, red below the horizon: the box's top and the ground face the blue.
    const auto scene = [](Scene3D& s) {
        no_sun(s);
        Sky sky;
        sky.zenith = {0.05f, 0.1f, 1.2f};
        sky.horizon = {0.3f, 0.3f, 0.3f};
        sky.ground = {1.2f, 0.1f, 0.05f};
        sky.ibl = true;
        s.set_sky(sky);
    };
    f.render(0.0f, false, scene);
    const Rgb top = f.at_world({0, 1.0f, 0.2f}), ground = f.at_world({2.5f, 0, 2.5f});
    INFO("top " << top.r << "," << top.g << "," << top.b << " ground " << ground.r << "," << ground.g << "," << ground.b);
    // The box is red-tinted albedo (0.9, 0.3, 0.2) yet lit blue from above; the grey ground reads blue.
    CHECK(ground.b > ground.r + 30);
    CHECK(top.b * 9 > top.r * 2);  // albedo 0.9 r vs 0.2 b: blue light keeps b well above a red-lit share
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a reflection probe puts the room into a mirror floor; captures only on change") {
    Fixture3D f;
    upload_probe_room(f);
    u32 version = 0;
    bool probe = false, ibl = true;
    const auto scene = [&](Scene3D& s) {
        no_sun(s);
        s.set_sky(constant_sky(0.05f, ibl));
        MeshInstance m;
        m.mesh = 7;
        s.add(m);
        m.mesh = 8;
        s.add(m);
        if (probe) {
            ReflectionProbe p = room_probe();
            p.version = version;
            s.add_reflection_probe(p);
        }
    };
    const Forward3D& fw = f.renderer.forward_3d();
    const vec3 spot{1.5f, 0.05f, 1.0f};
    f.render(0.0f, false, scene);
    const Rgb sky_only = f.at_world(spot);
    CHECK(fw.last_stats().probes == 0);

    probe = true;
    f.render(0.0f, false, scene);
    const Rgb with_probe = f.at_world(spot);
    INFO("sky " << sky_only.r << "," << sky_only.g << " probe " << with_probe.r << "," << with_probe.g);
    CHECK(fw.last_stats().probes == 1);
    CHECK(fw.last_stats().probes_captured == 1);
    CHECK(fw.last_stats().probe_draws > 0);
    CHECK(with_probe.r > sky_only.r + 60);  // the wall, mirrored
    CHECK(with_probe.r > with_probe.g + 60);

    f.render(0.0f, false, scene);
    CHECK(fw.last_stats().probes_captured == 0);  // nothing moved: the capture is reused
    const Rgb again = f.at_world(spot);
    CHECK(std::abs(again.r - with_probe.r) <= 2);
    version = 1;
    f.render(0.0f, false, scene);
    CHECK(fw.last_stats().probes_captured == 1);

    ibl = false;  // probes are an IBL feature: the hemisphere ambient ignores them
    f.render(0.0f, false, scene);
    CHECK(fw.last_stats().probes == 0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: an IBL texture lights the scene while the dome keeps the visible sky") {
    Fixture3D f;
    f.pitch_deg = 20.0f;
    // A uniform red equirect as the IBL source under a blue gradient sky.
    Array<u8> px;
    for (u32 i = 0; i < 16; ++i) {
        px.push_back(255);
        px.push_back(20);
        px.push_back(20);
        px.push_back(255);
    }
    const TextureMipUpload mip{4, 4, std::as_bytes(px.span())};
    TextureUpload up;
    up.mips = Span<const TextureMipUpload>(&mip, 1);
    const u32 red = f.renderer.textures().allocate_key();
    REQUIRE(f.renderer.textures().upload(red, 1, up).has_value());
    bool own = false;
    const auto scene = [&](Scene3D& s) {
        no_sun(s);
        Sky sky = constant_sky(0.0f, true);
        sky.zenith = sky.horizon = {0.1f, 0.2f, 1.0f};
        if (own) sky.ibl_texture = red;
        s.set_sky(sky);
    };
    const Forward3D& fw = f.renderer.forward_3d();
    const vec3 ground{2.0f, 0.0f, 2.0f}, above{0.0f, 4.0f, -20.0f};
    f.render(0.0f, false, scene);
    const Rgb ground_sky = f.at_world(ground), dome_sky = f.at_world(above);
    own = true;
    f.render(0.0f, false, scene);
    CHECK(fw.last_stats().ibl_baked);
    const Rgb ground_own = f.at_world(ground), dome_own = f.at_world(above);
    INFO("ground " << ground_sky.r << "," << ground_sky.b << " -> " << ground_own.r << "," << ground_own.b << "; dome "
                   << dome_sky.b << " -> " << dome_own.b);
    CHECK(ground_sky.b > ground_sky.r + 20);  // lit by the blue sky
    CHECK(ground_own.r > ground_own.b + 20);  // lit by the red IBL texture
    CHECK(std::abs(dome_own.b - dome_sky.b) <= 2);  // the dome still draws the visible sky
    CHECK(std::abs(dome_own.r - dome_sky.r) <= 2);
    f.render(0.0f, false, scene);
    CHECK_FALSE(fw.last_stats().ibl_baked);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a probe exported as RGBM and loaded back reflects what was captured") {
    Fixture3D f;
    upload_probe_room(f);
    bool wall = true;
    u32 baked = ~0u;
    const auto scene = [&](Scene3D& s) {
        no_sun(s);
        s.set_sky(constant_sky(0.05f, true));
        MeshInstance m;
        m.mesh = 7;
        s.add(m);
        if (wall) {
            m.mesh = 8;
            s.add(m);
        }
        ReflectionProbe p = room_probe();
        p.baked = baked;
        s.add_reflection_probe(p);
    };
    Forward3D& fw = f.renderer.forward_3d();
    const vec3 spot{1.5f, 0.05f, 1.0f};
    f.render(0.0f, false, scene);
    const Rgb captured = f.at_world(spot);
    CHECK(fw.last_stats().probes_captured == 1);

    // The editor's bake: the slot as one RGBM strip, read back and stored the way .mcenv is
    // (rows in texture order: read_pixels is top row first).
    const rhi::Extent2D es = Forward3D::probe_export_size();
    Array<u8> strip;
    strip.resize(usize(es.width) * es.height * 4);
    f.device->begin_frame(f.frame++);
    {
        rhi::CommandBuffer cmd;
        CHECK_FALSE(fw.record_probe_export(cmd, 1));  // slot 1 holds nothing
        REQUIRE(fw.record_probe_export(cmd, 0));
        f.device->submit(cmd);
    }
    REQUIRE(f.device->read_pixels(fw.probe_export_target(), rhi::Rect2D{0, 0, es.width, es.height}, strip.span()).has_value());
    f.device->end_frame();
    Array<u8> layers;
    layers.resize(strip.size());
    const usize row = usize(es.width) * 4;
    for (u32 y = 0; y < es.height; ++y) std::memcpy(&layers[y * row], &strip[(es.height - 1 - y) * row], row);
    const TextureMipUpload mip{Forward3D::kIblEnvWidth, Forward3D::kIblEnvWidth / 2, std::as_bytes(layers.span())};
    TextureUpload up;
    up.layers = Forward3D::kProbeLayers;
    up.mips = Span<const TextureMipUpload>(&mip, 1);
    baked = f.renderer.textures().allocate_key();
    REQUIRE(f.renderer.textures().upload(baked, 1, up).has_value());

    wall = false;  // gone from the scene, still in the bake
    f.render(0.0f, false, scene);
    CHECK(fw.last_stats().probes_loaded == 1);
    CHECK(fw.last_stats().probes_captured == 0);
    const Rgb loaded = f.at_world(spot);
    INFO("captured " << captured.r << "," << captured.g << " loaded " << loaded.r << "," << loaded.g);
    CHECK(std::abs(loaded.r - captured.r) <= 6);
    CHECK(std::abs(loaded.g - captured.g) <= 6);
    f.render(0.0f, false, scene);
    CHECK(fw.last_stats().probes_loaded == 0);  // decoded once

    baked = ~0u;  // without the bake the probe captures the wall-less room
    f.render(0.0f, false, scene);
    CHECK(fw.last_stats().probes_captured == 1);
    CHECK(f.at_world(spot).r + 60 < loaded.r);
    CHECK(f.device->validation_error_count() == 0);
}

// ---- screen-space ambient occlusion (M-VISUAL 3) ------------------------------------------------

namespace {

// Sun off, a grey hemisphere ambient and no sky reflections: the ambient is the whole frame.
void ambient_only(Scene3D& s) {
    no_sun(s);
    DirectionalLight sun = s.light();
    sun.ambient_sky = sun.ambient_ground = {0.6f, 0.6f, 0.6f};
    s.set_light(sun);
    Sky sky;
    sky.specular_ambient = 0.0f;
    s.set_sky(sky);
}

}  // namespace

TEST_CASE("render3d_gl: SSAO darkens the ground at the box's foot, not open ground") {
    Fixture3D f;
    bool on = false;
    const auto scene = [&](Scene3D& s) {
        ambient_only(s);
        AmbientOcclusion ao;
        ao.enabled = on;
        s.set_ambient_occlusion(ao);
    };
    const Forward3D& fw = f.renderer.forward_3d();
    const vec3 foot{0.0f, 0.0f, 0.58f}, open{2.5f, 0.0f, 2.5f}, top{0.0f, 1.0f, 0.0f};
    f.render(0.0f, false, scene);
    CHECK_FALSE(fw.last_stats().ssao);
    const Rgb foot_off = f.at_world(foot), open_off = f.at_world(open), top_off = f.at_world(top);
    on = true;
    f.render(0.0f, false, scene);
    CHECK(fw.last_stats().ssao);
    CHECK(fw.last_stats().ssao_draws == 2);  // the ground and the box
    const Rgb foot_on = f.at_world(foot), open_on = f.at_world(open), top_on = f.at_world(top);
    INFO("foot " << foot_off.luma() << " -> " << foot_on.luma() << ", open " << open_off.luma() << " -> " << open_on.luma()
                 << ", top " << top_off.luma() << " -> " << top_on.luma());
    CHECK(foot_on.luma() < foot_off.luma() - 15);         // the crease between the box and the ground
    CHECK(std::abs(open_on.luma() - open_off.luma()) <= 2);  // nothing near: nothing occluded
    CHECK(std::abs(top_on.luma() - top_off.luma()) <= 3);    // the box's open top
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: SSAO leaves direct light alone and off draws the frame without it") {
    Fixture3D f;
    bool on = false;
    const auto scene = [&](Scene3D& s) {  // the sun alone: no ambient for the occlusion to darken
        DirectionalLight sun = s.light();
        sun.ambient_sky = sun.ambient_ground = {0.0f, 0.0f, 0.0f};
        s.set_light(sun);
        Sky sky;
        sky.specular_ambient = 0.0f;
        s.set_sky(sky);
        AmbientOcclusion ao;
        ao.enabled = on;
        s.set_ambient_occlusion(ao);
    };
    f.render(0.0f, true, scene);
    const Array<u8> off = f.pixels;
    on = true;
    f.render(0.0f, true, scene);
    CHECK(f.renderer.forward_3d().last_stats().ssao);
    usize differing = 0;
    for (usize i = 0; i < off.size(); ++i) differing += f.pixels[i] != off[i];
    CHECK(differing == 0);
    on = false;
    f.render(0.0f, true, scene);
    differing = 0;
    for (usize i = 0; i < off.size(); ++i) differing += f.pixels[i] != off[i];
    CHECK(differing == 0);

    // The visibility texture: open (1) where nothing was drawn, darker in the box's crease.
    const rhi::TextureHandle ao = f.renderer.forward_3d().ssao();
    MCE_REQUIRE_OR_RETURN(!ao.is_null());
    const rhi::Extent2D size = f.device->texture_desc(ao).size;
    CHECK(size.width == kW / 2);
    CHECK(size.height == kH / 2);
    Array<u8> vis;
    vis.resize(usize(size.width) * size.height * 4);
    MCE_REQUIRE_OR_RETURN(f.device->read_pixels(ao, rhi::Rect2D{0, 0, size.width, size.height}, vis.span()).has_value());
    u8 lo = 255;
    for (usize i = 0; i < vis.size(); i += 4) lo = vis[i] < lo ? vis[i] : lo;
    CHECK(vis[0] == 255);  // a corner of open ground or sky
    CHECK(lo < 200);
    CHECK(f.device->validation_error_count() == 0);
}

// ---- screen-space reflections (M-VISUAL2 1) -------------------------------------------------------

namespace {

// The probe room (a mirror floor before an emissive wall) under a dim IBL sky, no sun, no probe:
// without SSR the floor reflects only the sky. `rough_floor` swaps in a roughness-0.8 floor.
void ssr_room(Scene3D& s, bool on, bool rough_floor = false) {
    no_sun(s);
    s.set_sky(constant_sky(0.05f, true));
    MeshInstance m;
    m.mesh = rough_floor ? 9 : 7;
    s.add(m);
    m.mesh = 8;
    s.add(m);
    ScreenSpaceReflections ssr;
    ssr.enabled = on;
    s.set_reflections(ssr);
}

usize differing_bytes(const Array<u8>& a, const Array<u8>& b) {
    usize n = 0;
    for (usize i = 0; i < a.size(); ++i) n += a[i] != b[i];
    return n;
}

}  // namespace

TEST_CASE("render3d_gl: SSR mirrors the emissive wall in the floor from the second frame") {
    Fixture3D f;
    upload_probe_room(f);
    const Forward3D& fw = f.renderer.forward_3d();
    const vec3 floor{2.0f, 0.05f, 0.5f};  // beside the box, in front of the wall: the wall's mirror image
    const vec3 sky_side{2.0f, 0.05f, 2.5f};  // nearer the camera: the mirror ray passes over the wall
    f.render(0.0f, false, [](Scene3D& s) { ssr_room(s, true); });  // the first frame: nothing to read yet
    CHECK(fw.last_stats().ssr);
    CHECK_FALSE(fw.last_stats().ssr_history);
    const Array<u8> first = f.pixels;
    f.render(0.0f, false, [](Scene3D& s) { ssr_room(s, false); });
    CHECK_FALSE(fw.last_stats().ssr);
    CHECK(differing_bytes(first, f.pixels) == 0);
    const Rgb floor_off = f.at_world(floor), near_off = f.at_world(sky_side);

    f.render(0.0f, false, [](Scene3D& s) { ssr_room(s, true); });
    CHECK(fw.last_stats().ssr_history);
    const Rgb floor_on = f.at_world(floor), near_on = f.at_world(sky_side);
    INFO("floor red " << floor_off.r << " -> " << floor_on.r << ", near " << near_off.luma() << " -> " << near_on.luma());
    CHECK(floor_on.r > floor_off.r + 100);  // the wall glows red
    CHECK(std::abs(near_on.luma() - near_off.luma()) <= 3);

    // The trace target: half size, some confident hits.
    const rhi::TextureHandle ssr = fw.ssr();
    MCE_REQUIRE_OR_RETURN(!ssr.is_null());
    const rhi::Extent2D size = f.device->texture_desc(ssr).size;
    CHECK(size.width == kW / 2);
    CHECK(size.height == kH / 2);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: SSR leaves rough surfaces and the frame with it off alone") {
    Fixture3D f;
    upload_probe_room(f);
    mesh_fixture::Builder floor;
    floor.add_node(-1);
    mesh_fixture::Box fb;
    fb.center = {0, 0.02f, 0};
    fb.half = {6, 0.03f, 6};
    floor.add_box(fb);
    floor.submeshes.back().material.metallic = 1.0f;
    floor.submeshes.back().material.roughness = 0.8f;  // above ScreenSpaceReflections::max_roughness
    REQUIRE(f.renderer.meshes().upload(9, 1, floor.build().span()).has_value());
    f.render(0.0f, false, [](Scene3D& s) { ssr_room(s, false, true); });
    const Array<u8> off = f.pixels;
    for (int i = 0; i < 2; ++i) f.render(0.0f, false, [](Scene3D& s) { ssr_room(s, true, true); });
    CHECK(f.renderer.forward_3d().last_stats().ssr_history);
    CHECK(differing_bytes(off, f.pixels) == 0);
    f.render(0.0f, false, [](Scene3D& s) { ssr_room(s, false, true); });
    CHECK_FALSE(f.renderer.forward_3d().last_stats().ssr);
    CHECK(differing_bytes(off, f.pixels) == 0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: SSR shares the prepass with SSAO") {
    Fixture3D f;
    upload_probe_room(f);
    f.render(0.0f, false, [](Scene3D& s) {
        ssr_room(s, true);
        AmbientOcclusion ao;
        ao.enabled = true;
        s.set_ambient_occlusion(ao);
    });
    const Forward3DStats& st = f.renderer.forward_3d().last_stats();
    CHECK(st.ssr);
    CHECK(st.ssao);
    CHECK(st.ssao_draws == 4);  // ground, box, floor, wall: drawn once for both
    CHECK(f.device->validation_error_count() == 0);
}

// ---- local-light shadows (M-VISUAL 4) -----------------------------------------------------------------

namespace {

// No sun, a dim ambient, 256-texel tiles (16 per 1024² layer), and `lights` added after.
void local_lit(Scene3D& s, bool shadows, Span<const PointLight> lights) {
    no_sun(s);
    DirectionalLight sun = s.light();
    sun.ambient_sky = sun.ambient_ground = {0.05f, 0.05f, 0.05f};
    s.set_light(sun);
    Sky sky;
    sky.specular_ambient = 0.0f;
    s.set_sky(sky);
    LocalShadowSettings ls;
    ls.tile = 256;
    s.set_local_shadows(ls);
    for (PointLight l : lights) {
        l.cast_shadows = shadows;
        s.add_point_light(l);
    }
}

PointLight spot_light() {
    PointLight l;
    l.position = {-3.0f, 3.0f, 0.0f};
    l.direction = {1.0f, -1.0f, 0.0f};
    l.radius = 12.0f;
    l.intensity = 25.0f;
    l.spot_inner = math::radians(30.0f);
    l.spot_outer = math::radians(40.0f);
    return l;
}

PointLight point_light() {
    PointLight l;
    l.position = {-1.5f, 1.2f, 0.0f};
    l.radius = 8.0f;
    l.intensity = 8.0f;
    return l;
}

}  // namespace

TEST_CASE("render3d_gl: a shadowed spot light leaves the floor behind the box dark, the rest as lit") {
    Fixture3D f;
    bool shadows = false;
    const PointLight light = spot_light();
    const auto scene = [&](Scene3D& s) { local_lit(s, shadows, Span<const PointLight>(&light, 1)); };
    const vec3 behind{1.4f, 0.0f, 0.0f}, lit{0.0f, 0.0f, 1.5f}, face{-0.5f, 0.5f, 0.0f};
    f.render(0.0f, false, scene);
    const Forward3D& fw = f.renderer.forward_3d();
    CHECK(fw.last_stats().local_shadow_tiles == 0);
    const Rgb behind_off = f.at_world(behind), lit_off = f.at_world(lit), face_off = f.at_world(face);
    shadows = true;
    f.render(0.0f, false, scene);
    CHECK(fw.last_stats().local_shadow_tiles == 1);
    const Rgb behind_on = f.at_world(behind), lit_on = f.at_world(lit), face_on = f.at_world(face);
    INFO("behind " << behind_off.luma() << " -> " << behind_on.luma() << ", lit " << lit_off.luma() << " -> "
                   << lit_on.luma() << ", face " << face_off.luma() << " -> " << face_on.luma());
    CHECK(behind_off.luma() > 40);                          // the cone reaches it
    CHECK(behind_on.luma() < behind_off.luma() / 3);       // the box's shadow
    CHECK(std::abs(lit_on.luma() - lit_off.luma()) <= 2);  // open floor: no acne
    CHECK(std::abs(face_on.luma() - face_off.luma()) <= 2);  // the box's lit face
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a shadowed point light casts through its cube faces without acne at the seams") {
    Fixture3D f;
    bool shadows = false;
    const PointLight light = point_light();
    const auto scene = [&](Scene3D& s) { local_lit(s, shadows, Span<const PointLight>(&light, 1)); };
    // Floor points seen through the -Y face (under the light), the -X and +Z faces and their seams.
    const vec3 lit[] = {{-1.5f, 0.0f, 0.3f}, {-3.0f, 0.0f, 0.0f}, {-1.5f, 0.0f, 1.6f}, {-2.7f, 0.0f, 1.2f}, {-0.3f, 0.0f, 1.2f}};
    const vec3 behind{1.5f, 0.0f, 0.0f};
    f.render(0.0f, false, scene);
    Rgb lit_off[5];
    for (int i = 0; i < 5; ++i) lit_off[i] = f.at_world(lit[i]);
    const Rgb behind_off = f.at_world(behind);
    shadows = true;
    f.render(0.0f, false, scene);
    const Forward3D& fw = f.renderer.forward_3d();
    CHECK(fw.last_stats().local_shadow_tiles == 6);
    for (int i = 0; i < 5; ++i) {
        const Rgb on = f.at_world(lit[i]);
        INFO("lit " << i << ": " << lit_off[i].luma() << " -> " << on.luma());
        CHECK(lit_off[i].luma() > 20);
        CHECK(std::abs(on.luma() - lit_off[i].luma()) <= 2);
    }
    const Rgb behind_on = f.at_world(behind);
    INFO("behind " << behind_off.luma() << " -> " << behind_on.luma());
    CHECK(behind_off.luma() > 20);
    CHECK(behind_on.luma() < behind_off.luma() / 3);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: local shadows turned off draw the unshadowed frame") {
    Fixture3D f;
    const PointLight lights[2] = {spot_light(), point_light()};
    f.render(0.0f, false, [&](Scene3D& s) { local_lit(s, false, Span<const PointLight>(lights, 2)); });
    const Array<u8> off = f.pixels;
    f.render(0.0f, false, [&](Scene3D& s) {
        local_lit(s, true, Span<const PointLight>(lights, 2));
        LocalShadowSettings ls = s.local_shadow_settings();
        ls.enabled = false;
        s.set_local_shadows(ls);
    });
    CHECK(f.renderer.forward_3d().last_stats().local_shadow_tiles == 0);
    usize differing = 0;
    for (usize i = 0; i < off.size(); ++i) differing += f.pixels[i] != off[i];
    CHECK(differing == 0);
    // A settings/renderer mismatch falls back to unshadowed rather than reading the wrong layers.
    f.render(0.0f, false, [&](Scene3D& s) {
        local_lit(s, true, Span<const PointLight>(lights, 2));
        LocalShadowSettings ls = s.local_shadow_settings();
        ls.layers = 2;
        s.set_local_shadows(ls);
    });
    CHECK(f.renderer.forward_3d().last_stats().local_shadow_tiles == 0);
    differing = 0;
    for (usize i = 0; i < off.size(); ++i) differing += f.pixels[i] != off[i];
    CHECK(differing == 0);
}

TEST_CASE("render3d_gl: local shadow tiles go to the most important lights first") {
    Fixture3D f;
    Scene3D s(f.renderer.meshes(), mem::heap());
    s.begin(orbit_camera({0, 0.5f, 0}, 0.0f, math::radians(35.0f), 7.0f), {kW, kH});
    ShadowSettings ss;
    ss.resolution = 1024;
    s.set_shadows(ss);
    LocalShadowSettings ls;
    ls.tile = 256;  // 16 tiles
    s.set_local_shadows(ls);
    MeshInstance box;
    box.mesh = 1;
    s.add(box);
    const auto add = [&](vec3 p, f32 intensity, bool spot, bool cast = true) {
        PointLight l;
        l.position = p;
        l.radius = 4.0f;
        l.intensity = intensity;
        l.cast_shadows = cast;
        if (spot) l.spot_outer = math::radians(30.0f);
        s.add_point_light(l);
    };
    add({0, 2, 0}, 50.0f, false);          // 0: point, 6 tiles
    add({1, 2, 0}, 1.0f, true);            // 1: spot, the dimmest
    add({-1, 2, 0}, 100.0f, false);        // 2: point, the brightest
    add({0, 2, -1}, 40.0f, false);         // 3: point, ranked after spot 4 (farther, dimmer): 4 tiles left, dropped
    add({0, 2, 1}, 30.0f, true);           // 4: spot
    add({0, 2, 2}, 500.0f, false, false);  // 5: no shadows asked
    add({0, 2, 40}, 500.0f, true);         // 6: behind the camera: nothing on screen to shadow
    s.build();
    const Scene3D::Stats& st = s.stats();
    CHECK(st.local_shadow_lights == 4);
    CHECK(st.local_shadow_tiles == 14);
    CHECK(st.local_shadows_dropped == 1);
    const Span<const LocalShadowTile> tiles = s.local_shadow_tiles();
    MCE_REQUIRE_OR_RETURN(tiles.size() == 14);
    // By importance: light 2 (6 faces), light 0 (6), spot 4, light 3 (needs 6 of 3: dropped), spot 1.
    for (u32 i = 0; i < 6; ++i) CHECK(tiles[i].light == 2);
    for (u32 i = 6; i < 12; ++i) CHECK(tiles[i].light == 0);
    CHECK(tiles[12].light == 4);
    CHECK(tiles[13].light == 1);
    for (u32 i = 0; i < 14; ++i) {
        CHECK(tiles[i].layer == 0);
        CHECK(tiles[i].rect.width == 256);
        for (u32 j = 0; j < i; ++j) CHECK_FALSE((tiles[i].rect.x == tiles[j].rect.x && tiles[i].rect.y == tiles[j].rect.y));
    }
    const Span<const GpuPointLight> pool = s.cluster_setup().lights();
    CHECK(pool[2].shadow.x == 0.0f);
    CHECK(pool[2].shadow.y == 6.0f);
    CHECK(pool[0].shadow.x == 6.0f);
    CHECK(pool[3].shadow.x == -1.0f);
    CHECK(pool[4].shadow.x == 12.0f);
    CHECK(pool[4].shadow.y == 1.0f);
    CHECK(pool[5].shadow.x == -1.0f);
    CHECK(pool[6].shadow.x == -1.0f);
    // The box sits under every light: each spot tile and the -Y faces list it as a caster.
    CHECK(tiles[12].caster_count == 1);
    CHECK(tiles[0 + 3].caster_count == 1);  // light 2's -Y face

    // max_lights caps the count whatever room is left.
    Scene3D s2(f.renderer.meshes(), mem::heap());
    s2.begin(orbit_camera({0, 0.5f, 0}, 0.0f, math::radians(35.0f), 7.0f), {kW, kH});
    s2.set_shadows(ss);
    ls.max_lights = 1;
    s2.set_local_shadows(ls);
    PointLight a;
    a.position = {0, 2, 0};
    a.cast_shadows = true;
    a.spot_outer = 0.5f;
    s2.add_point_light(a);
    a.intensity = 2.0f;
    s2.add_point_light(a);
    s2.build();
    CHECK(s2.stats().local_shadow_lights == 1);
    CHECK(s2.stats().local_shadows_dropped == 1);
    CHECK(s2.local_shadow_tiles()[0].light == 1);
}

// ---- temporal anti-aliasing (M-VISUAL 5) --------------------------------------------------------------

namespace {

f64 mean_abs_diff(const Array<u8>& a, const Array<u8>& b) {
    u64 sum = 0;
    for (usize i = 0; i < a.size(); i += 4)
        for (usize c = 0; c < 3; ++c) sum += u64(std::abs(int(a[i + c]) - int(b[i + c])));
    const usize channels = a.size() / 4 * 3;
    return f64(sum) / f64(channels);
}

void post_aa(Scene3D& s, bool taa) {
    PostFx post;
    post.bloom = false;
    post.fxaa = false;
    post.taa = taa;
    s.set_post(post);
}

// The still frame with 4x MSAA: the reference the anti-aliased frames are measured against.
Array<u8> msaa_reference() {
    Fixture3D f(4);
    f.render(0.0f, true, [](Scene3D& s) { post_aa(s, false); });
    return f.pixels;
}

// A second box at z = 1.5 sliding along x (mesh 50), with or without its previous world.
void upload_slider(Fixture3D& f) {
    mesh_fixture::Builder b;
    b.add_node(-1);
    mesh_fixture::Box box;
    box.center = {0, 0.4f, 0};
    box.half = {0.4f, 0.4f, 0.4f};
    box.color[0] = 0.2f;
    box.color[1] = 0.4f;
    box.color[2] = 0.9f;
    b.add_box(box);
    REQUIRE(f.renderer.meshes().upload(50, 1, b.build().span()).has_value());
}

}  // namespace

TEST_CASE("render3d_gl: TAA off draws today's frame, and a TAA run in between leaves no trace") {
    Fixture3D f(1);
    bool taa = false;
    const auto scene = [&](Scene3D& s) { post_aa(s, taa); };
    f.render(0.0f, true, scene);
    const Array<u8> off = f.pixels;
    const Forward3D& fw = f.renderer.forward_3d();
    CHECK_FALSE(fw.last_stats().taa);
    taa = true;
    for (int i = 0; i < 3; ++i) f.render(0.0f, true, scene);
    CHECK(fw.last_stats().taa);
    CHECK(fw.last_stats().taa_history);
    CHECK(fw.last_stats().velocity_draws == 2);
    CHECK_FALSE(fw.last_stats().fxaa);
    taa = false;
    f.render(0.0f, true, scene);
    usize differing = 0;
    for (usize i = 0; i < off.size(); ++i) differing += f.pixels[i] != off[i];
    CHECK(differing == 0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: TAA over a still frame converges toward the multisampled edges") {
    const Array<u8> ref = msaa_reference();
    Fixture3D f(1);
    f.render(0.0f, true, [](Scene3D& s) { post_aa(s, false); });
    // The edges: pixels where the aliased frame is off the reference by more than a few levels.
    const Array<u8> aliased_px = f.pixels;
    Array<u8> edge;
    edge.resize(ref.size() / 4);
    usize edges = 0;
    for (usize p = 0; p < edge.size(); ++p) {
        int d = 0;
        for (usize c = 0; c < 3; ++c) d = std::max(d, std::abs(int(aliased_px[p * 4 + c]) - int(ref[p * 4 + c])));
        edge[p] = d > 12;
        edges += edge[p];
    }
    const auto edge_error = [&](const Array<u8>& px) {
        u64 sum = 0;
        for (usize p = 0; p < edge.size(); ++p)
            if (edge[p])
                for (usize c = 0; c < 3; ++c) sum += u64(std::abs(int(px[p * 4 + c]) - int(ref[p * 4 + c])));
        return f64(sum) / f64(std::max<usize>(edges, 1) * 3);
    };
    for (int i = 0; i < 16; ++i) f.render(0.0f, true, [](Scene3D& s) { post_aa(s, true); });
    const f64 aliased = edge_error(aliased_px), taa = edge_error(f.pixels);
    INFO(edges << " edge pixels; mean |diff| to 4x MSAA there: aliased " << aliased << ", TAA " << taa);
    CHECK(edges > 50);
    CHECK(taa < aliased * 0.6);
    // Flat open ground keeps its shade (no blur, no drift).
    const Rgb open = f.at_world({2.5f, 0.0f, 2.5f});
    f.render(0.0f, true, [](Scene3D& s) { post_aa(s, false); });
    CHECK(std::abs(f.at_world({2.5f, 0.0f, 2.5f}).luma() - open.luma()) <= 2);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: TAA follows a moving box through its velocity without a trail") {
    const auto run = [](bool velocity, bool taa) {
        Fixture3D f(1);
        upload_slider(f);
        math::mat4 prev = math::translation({-1.8f, 0.0f, 1.5f});
        for (int k = 0; k < 12; ++k) {
            const math::mat4 world = math::translation({-1.8f + 0.3f * f32(k), 0.0f, 1.5f});
            f.render(0.0f, true, [&](Scene3D& s) {
                post_aa(s, taa);
                MeshInstance m;
                m.mesh = 50;
                m.world = world;
                if (velocity) m.prev_world = &prev;
                s.add(m);
            });
            prev = world;
        }
        return f.pixels;
    };
    const Array<u8> still = run(false, false);  // the last position without TAA
    const f64 with_velocity = mean_abs_diff(run(true, true), still);
    const f64 without = mean_abs_diff(run(false, true), still);
    INFO("mean |diff| to the un-anti-aliased last frame: with velocity " << with_velocity << ", without " << without);
    MESSAGE("moving box, mean |diff|: with velocity " << with_velocity << ", without " << without);
    CHECK(with_velocity < without);
    CHECK(with_velocity < 3.0);
}

TEST_CASE("render3d_gl: a skinned pose's previous joints feed the velocity") {
    Fixture3D f(1);
    mesh_fixture::Builder arm;
    const u32 root = arm.add_node(-1);
    const u32 a = arm.add_node(static_cast<i32>(root));
    const u32 b = arm.add_node(static_cast<i32>(a), {0, 1, 0});
    for (f32 y : {0.5f, 1.5f}) {
        mesh_fixture::Box seg;
        seg.center = {0, y, 0};
        seg.half = {0.2f, 0.5f, 0.2f};
        seg.node = root;
        seg.color[1] = 0.8f;
        arm.add_box(seg);
    }
    arm.skin_submesh(0, {a, b}, [](vec3) { return 0u; });
    arm.skin_submesh(1, {a, b}, [](vec3) { return 1u; });
    REQUIRE(f.renderer.meshes().upload(3, 1, arm.build().span()).has_value());
    const GpuMesh& m = *f.renderer.meshes().get(3);
    math::mat4 pose[3], prev[3];
    pose_nodes(m, ~0u, 0.0f, Span<math::mat4>(pose, 3));
    pose_nodes(m, ~0u, 0.0f, Span<math::mat4>(prev, 3));
    prev[b] = prev[a] * math::trs({0, 1, 0}, math::quat_from_axis_angle({0, 0, 1}, 0.3f), {1, 1, 1});
    for (bool taa : {false, true}) {
        Scene3D scene(f.renderer.meshes(), mem::heap());
        scene.begin(orbit_camera({0, 1, 0}, 0.0f, math::radians(10.0f), 6.0f), {kW, kH});
        post_aa(scene, taa);
        MeshInstance inst;
        inst.mesh = 3;
        inst.pose = Span<const math::mat4>(pose, 3);
        inst.prev_pose = Span<const math::mat4>(prev, 3);
        scene.add(inst);
        scene.build();
        // TAA adds last frame's joints after this frame's; the draws point at both.
        CHECK(scene.joint_palette().size() == (taa ? 8u : 4u));
        for (const Scene3D::Draw& d : scene.draws())
            if (d.palette != ~0u) CHECK(d.prev_palette == (taa ? d.palette + 4 : d.palette));
        f.device->begin_frame(f.frame++);
        rhi::CommandBuffer cmd;
        f.renderer.record_3d(cmd, scene, f.target);
        f.device->submit(cmd);
        f.device->end_frame();
    }
    CHECK(f.renderer.forward_3d().last_stats().velocity_draws == 2);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: temporal upscaling keeps a half-scale frame closer to full resolution") {
    const Array<u8> ref = msaa_reference();
    Fixture3D f(1, true, true, 0.5f);
    f.render(0.0f, true, [](Scene3D& s) { post_aa(s, false); });
    const f64 bilinear = mean_abs_diff(f.pixels, ref);
    for (int i = 0; i < 24; ++i) f.render(0.0f, true, [](Scene3D& s) { post_aa(s, true); });
    const f64 upscaled = mean_abs_diff(f.pixels, ref);
    INFO("mean |diff| to full-res 4x MSAA: bilinear " << bilinear << ", temporal " << upscaled);
    MESSAGE("mean |diff| to full-res 4x MSAA: bilinear " << bilinear << ", temporal " << upscaled);
    CHECK(upscaled < bilinear * 0.75);
    const Forward3D& fw = f.renderer.forward_3d();
    CHECK(f.device->texture_desc(fw.velocity()).size.width == kW / 2);
    CHECK(f.device->texture_desc(fw.taa_history()).size.width == kW);
    CHECK(f.device->validation_error_count() == 0);
}

// ---- spatial upsample and sharpening (M-VISUAL2 2) ----------------------------------------------

namespace {

void finish_post(Scene3D& s, Upscaler upscaler, f32 sharpen) {
    PostFx post;
    post.bloom = false;
    post.fxaa = false;
    post.upscaler = upscaler;
    post.sharpen = sharpen;
    s.set_post(post);
}

// Sum of squared horizontal and vertical luma steps: higher = crisper edges.
f64 edge_energy(const Array<u8>& px) {
    const auto luma = [&](u32 x, u32 y) {
        const usize i = (usize(y) * kW + x) * 4;
        return 0.2126 * px[i] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2];
    };
    f64 e = 0.0;
    for (u32 y = 0; y + 1 < kH; ++y)
        for (u32 x = 0; x + 1 < kW; ++x) {
            const f64 dx = luma(x + 1, y) - luma(x, y), dy = luma(x, y + 1) - luma(x, y);
            e += dx * dx + dy * dy;
        }
    return e;
}

}  // namespace

TEST_CASE("render3d_gl: the spatial upsample brings a half-scale frame nearer full resolution than bilinear") {
    const Array<u8> ref = msaa_reference();
    Fixture3D f(1, true, true, 0.5f);
    f.render(0.0f, true, [](Scene3D& s) { post_aa(s, false); });  // today's path: the tonemap upsamples
    const Array<u8> today = f.pixels;
    f.render(0.0f, true, [](Scene3D& s) { finish_post(s, Upscaler::Bilinear, 0.0f); });
    CHECK_FALSE(f.renderer.forward_3d().last_stats().upscaled);
    CHECK(differing_bytes(today, f.pixels) == 0);
    f.render(0.0f, true, [](Scene3D& s) { finish_post(s, Upscaler::Spatial, 0.0f); });
    CHECK(f.renderer.forward_3d().last_stats().upscaled);
    CHECK_FALSE(f.renderer.forward_3d().last_stats().sharpened);
    const Array<u8> spatial = f.pixels;
    const f64 e_bilinear = edge_energy(today), e_spatial = edge_energy(spatial);
    const f64 d_bilinear = mean_abs_diff(today, ref), d_spatial = mean_abs_diff(spatial, ref);
    MESSAGE("half scale: edge energy bilinear " << e_bilinear << ", spatial " << e_spatial << "; mean |diff| to 4x MSAA bilinear "
                                                << d_bilinear << ", spatial " << d_spatial);
    CHECK(d_spatial < d_bilinear * 0.9);  // nearer the full-resolution frame than the bilinear blur
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: sharpening crisps a full-size frame; spatial at full size and sharpen 0 change nothing") {
    Fixture3D f(1);
    f.render(0.0f, true, [](Scene3D& s) { finish_post(s, Upscaler::Bilinear, 0.0f); });
    const Array<u8> plain = f.pixels;
    f.render(0.0f, true, [](Scene3D& s) { finish_post(s, Upscaler::Spatial, 0.0f); });
    CHECK_FALSE(f.renderer.forward_3d().last_stats().upscaled);  // nothing to upsample
    CHECK(differing_bytes(plain, f.pixels) == 0);
    f.render(0.0f, true, [](Scene3D& s) { finish_post(s, Upscaler::Bilinear, 0.8f); });
    CHECK(f.renderer.forward_3d().last_stats().sharpened);
    const f64 e_plain = edge_energy(plain), e_sharp = edge_energy(f.pixels);
    MESSAGE("full size: edge energy plain " << e_plain << ", sharpened " << e_sharp << ", mean |diff| " << mean_abs_diff(plain, f.pixels));
    CHECK(e_sharp > e_plain * 1.05);
    CHECK(mean_abs_diff(plain, f.pixels) < 4.0);  // a sharpen, not a different picture
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: sharpening follows the spatial upsample; r.upscaler and r.sharpen override the scene") {
    Fixture3D h(1, true, true, 0.5f);
    h.render(0.0f, true, [](Scene3D& s) { finish_post(s, Upscaler::Spatial, 0.5f); });
    CHECK(h.renderer.forward_3d().last_stats().upscaled);
    CHECK(h.renderer.forward_3d().last_stats().sharpened);
    ConVar* up = ConVar::find(StringId("r.upscaler"));
    ConVar* sh = ConVar::find(StringId("r.sharpen"));
    MCE_REQUIRE_OR_RETURN(up && sh);
    CHECK(up->set_string("bilinear"));
    CHECK(sh->set_string("0"));
    h.render(0.0f, true, [](Scene3D& s) { finish_post(s, Upscaler::Spatial, 0.5f); });
    CHECK_FALSE(h.renderer.forward_3d().last_stats().upscaled);
    CHECK_FALSE(h.renderer.forward_3d().last_stats().sharpened);
    CHECK(up->set_string("spatial"));
    h.render(0.0f, true, [](Scene3D& s) { finish_post(s, Upscaler::Bilinear, 0.0f); });
    CHECK(h.renderer.forward_3d().last_stats().upscaled);
    up->reset();
    sh->reset();
    CHECK(h.device->validation_error_count() == 0);
}

// ---- global illumination: the irradiance volume (M-VISUAL 6) -------------------------------------

TEST_CASE("render3d_gl: GI off draws today's frame; a GI run in between and the Low tier leave no trace") {
    Fixture3D f;
    upload_probe_room(f);
    bool gi = false, updates = true;
    const auto scene = [&](Scene3D& s) {
        gi_wall_scene(s, gi);
        if (gi) {
            GiVolume g = s.gi_volume();
            g.updates = updates;
            s.set_gi_volume(g);
        }
    };
    const Forward3D& fw = f.renderer.forward_3d();
    f.render(0.0f, true, scene);
    const Array<u8> off = f.pixels;
    CHECK_FALSE(fw.last_stats().gi);
    gi = true;
    for (int i = 0; i < 2; ++i) f.render(0.0f, true, scene);  // 48 probes fill over two frames
    CHECK(fw.last_stats().gi);
    CHECK(differing_bytes(f.pixels, off) > 0);
    gi = false;
    f.render(0.0f, true, scene);
    CHECK(differing_bytes(f.pixels, off) == 0);
    // The Low tier without a bake: no runtime captures, so no volume at all.
    gi = true;
    updates = false;
    f.render(0.0f, true, scene);
    CHECK_FALSE(fw.last_stats().gi);
    CHECK(fw.last_stats().gi_probes == 0);
    CHECK(differing_bytes(f.pixels, off) == 0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the volume bleeds an emissive wall's color onto the ground; fill once, then roll") {
    Fixture3D f;
    upload_probe_room(f);
    bool gi = false;
    const auto scene = [&](Scene3D& s) { gi_wall_scene(s, gi); };
    const Forward3D& fw = f.renderer.forward_3d();
    const vec3 spot{1.5f, 0.0f, -0.5f};  // the ground in front of the wall, beside the box
    f.render(0.0f, true, scene);
    const Rgb sky_only = f.at_world(spot);
    gi = true;
    f.render(0.0f, true, scene);  // the fill's first 32 probes: the frame keeps the sky's ambient
    CHECK_FALSE(fw.last_stats().gi);
    CHECK_FALSE(fw.last_stats().gi_filled);
    CHECK(fw.last_stats().gi_probes == 32);
    CHECK(f.at_world(spot).r == sky_only.r);
    f.render(0.0f, true, scene);
    const Rgb bled = f.at_world(spot);
    INFO("sky " << sky_only.r << "," << sky_only.g << "," << sky_only.b << " gi " << bled.r << "," << bled.g << "," << bled.b);
    MESSAGE("ground in front of the red wall: sky ambient " << sky_only.r << "," << sky_only.g << "," << sky_only.b
                                                            << ", volume " << bled.r << "," << bled.g << "," << bled.b);
    CHECK(fw.last_stats().gi);
    CHECK(fw.last_stats().gi_filled);
    CHECK(fw.last_stats().gi_probes == 16);  // the rest
    CHECK(fw.last_stats().gi_draws > 0);
    CHECK(bled.r > sky_only.r + 30);
    CHECK(bled.r > bled.g + 30);
    CHECK(bled.r > bled.b + 30);
    f.render(0.0f, true, scene);
    CHECK_FALSE(fw.last_stats().gi_filled);
    CHECK(fw.last_stats().gi_probes == 8);  // two rows of four
    const rhi::TextureDesc& vd = f.device->texture_desc(fw.gi_volume());
    CHECK(vd.size.width == 16);
    CHECK(vd.size.height == 12);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: sunlight bounces off the ground onto the box's unlit face") {
    Fixture3D f;
    bool gi = false;
    const auto scene = [&](Scene3D& s) {
        DirectionalLight sun;
        sun.direction = {0.3f, -1.0f, 0.9f};  // lights the box's back (-z) face, not the one the camera sees
        sun.intensity = 3.0f;
        sun.ambient_sky = sun.ambient_ground = {0, 0, 0};
        s.set_light(sun);
        Sky sky;
        sky.enabled = false;
        sky.specular_ambient = 0.0f;
        s.set_sky(sky);
        Fog fog;
        fog.color = {0, 0, 0};
        s.set_fog(fog);
        if (gi) s.set_gi_volume(room_gi());
    };
    const vec3 face{0.2f, 0.4f, 0.5f};  // the +z face, toward the camera
    f.render(0.0f, true, scene);
    const Rgb dark = f.at_world(face);
    gi = true;
    for (int i = 0; i < 8; ++i) f.render(0.0f, true, scene);  // a bounce more on every sweep
    const Rgb bounced = f.at_world(face);
    INFO("unlit face " << dark.luma() << " -> " << bounced.luma());
    MESSAGE("box face away from the sun: luma " << dark.luma() << " without the volume, " << bounced.luma() << " with it");
    CHECK(dark.luma() < 8);
    CHECK(bounced.luma() > dark.luma() + 15);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: rolling updates carry a light switched on into the volume") {
    Fixture3D f;
    upload_probe_room(f);
    bool lamp = false;
    u32 version = 0;
    const auto scene = [&](Scene3D& s) {
        gi_wall_scene(s, true);
        GiVolume g = s.gi_volume();
        g.hysteresis = 0.5f;
        g.version = version;
        s.set_gi_volume(g);
        if (lamp) {
            PointLight pl;
            pl.position = {-1.5f, 0.6f, 1.5f};
            pl.color = {0.2f, 1.0f, 0.2f};
            pl.intensity = 30.0f;
            pl.radius = 2.5f;  // short of the spot: what reaches it is bounced
            s.add_point_light(pl);
        }
    };
    const Forward3D& fw = f.renderer.forward_3d();
    const vec3 spot{2.0f, 0.0f, 1.5f};
    for (int i = 0; i < 2; ++i) f.render(0.0f, true, scene);
    lamp = true;
    f.render(0.0f, true, scene);
    CHECK(fw.last_stats().gi_probes == 8);
    const Rgb first = f.at_world(spot);
    for (int i = 0; i < 24; ++i) f.render(0.0f, true, scene);  // four sweeps of the 12 rows
    const Rgb rolled = f.at_world(spot);
    version = 1;  // the reference: every probe captured with the lamp on
    for (int i = 0; i < 2; ++i) f.render(0.0f, true, scene);
    CHECK(fw.last_stats().gi_filled);
    const Rgb filled = f.at_world(spot);
    INFO("first " << first.g << " rolled " << rolled.g << " filled " << filled.g);
    MESSAGE("green at the far ground after the lamp: first frame " << first.g << ", 24 frames " << rolled.g << ", refilled " << filled.g);
    CHECK(filled.g > first.g + 6);
    CHECK(std::abs(rolled.g - filled.g) * 3 < filled.g - first.g);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a crowded volume's captures stay inside the frame's uniform ring") {
    Fixture3D f;
    upload_probe_room(f);
    const auto scene = [&](Scene3D& s) {
        gi_wall_scene(s, true);
        // 400 posts through the volume: a face sees up to hundreds of draws.
        for (int z = 0; z < 20; ++z)
            for (int x = 0; x < 20; ++x) {
                MeshInstance m;
                m.mesh = 1;
                m.world = math::translation({-2.85f + 0.3f * f32(x), 0.0f, -2.85f + 0.3f * f32(z)}) *
                          math::scaling({0.12f, 0.6f, 0.12f});
                m.cast_shadows = false;
                s.add(m);
            }
    };
    const Forward3D& fw = f.renderer.forward_3d();
    u32 frames = 0;
    for (; frames < 64 && !fw.last_stats().gi_filled; ++frames) {
        f.render(0.0f, true, scene);
        CHECK(f.gpu.ring_overflows == 0);
        CHECK(fw.last_stats().gi_probes > 0);  // a probe a frame at least
        CHECK(fw.last_stats().gi_draws <= 6 * Forward3D::kGiMaxFaceDraws * fw.last_stats().gi_probes);
    }
    MESSAGE("crowded fill: " << frames << " frames, last frame " << fw.last_stats().gi_draws << " capture draws, "
                             << f.gpu.uniform_bytes << " uniform bytes");
    CHECK(fw.last_stats().gi_filled);
    CHECK(frames > 2);  // the uniform budget, not kGiMaxBatchProbes, paced the fill
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a volume exported as RGBM and loaded back lights like the capture; Low uses the bake") {
    Fixture3D f;
    upload_probe_room(f);
    bool updates = true;
    u32 baked = ~0u;
    const auto scene = [&](Scene3D& s) {
        gi_wall_scene(s, false);
        GiVolume g = room_gi();
        g.baked = baked;
        g.updates = updates;
        s.set_gi_volume(g);
    };
    Forward3D& fw = f.renderer.forward_3d();
    const vec3 spot{1.5f, 0.0f, -0.5f};
    for (int i = 0; i < 2; ++i) f.render(0.0f, true, scene);
    const Rgb captured = f.at_world(spot);
    CHECK(fw.last_stats().gi_filled);

    const rhi::Extent2D es = gi_volume_size(room_gi());
    Array<u8> strip;
    strip.resize(usize(es.width) * es.height * 4);
    f.device->begin_frame(f.frame++);
    {
        rhi::CommandBuffer cmd;
        REQUIRE(fw.record_gi_export(cmd));
        f.device->submit(cmd);
    }
    REQUIRE(f.device->read_pixels(fw.gi_export_target(), rhi::Rect2D{0, 0, es.width, es.height}, strip.span()).has_value());
    f.device->end_frame();
    Array<u8> texels;
    texels.resize(strip.size());
    const usize row = usize(es.width) * 4;
    for (u32 y = 0; y < es.height; ++y) std::memcpy(&texels[y * row], &strip[(es.height - 1 - y) * row], row);
    const TextureMipUpload mip{es.width, es.height, std::as_bytes(texels.span())};
    TextureUpload up;
    up.layers = 1;
    up.mips = Span<const TextureMipUpload>(&mip, 1);
    baked = f.renderer.textures().allocate_key();
    REQUIRE(f.renderer.textures().upload(baked, 1, up).has_value());

    updates = false;  // the Low tier: the bake, never a capture
    f.render(0.0f, true, scene);
    CHECK(fw.last_stats().gi);
    CHECK(fw.last_stats().gi_loaded);
    CHECK(fw.last_stats().gi_probes == 0);
    const Rgb loaded = f.at_world(spot);
    INFO("captured " << captured.r << "," << captured.g << " loaded " << loaded.r << "," << loaded.g);
    CHECK(std::abs(loaded.r - captured.r) <= 6);
    CHECK(std::abs(loaded.g - captured.g) <= 6);
    f.render(0.0f, true, scene);
    CHECK_FALSE(fw.last_stats().gi_loaded);  // decoded once
    CHECK(fw.last_stats().gi_probes == 0);
    updates = true;  // the higher tiers start from the bake and keep rolling
    f.render(0.0f, true, scene);
    CHECK(fw.last_stats().gi_probes == 8);
    CHECK(f.device->validation_error_count() == 0);
}

// ---- atmosphere and volumetric fog (M-VISUAL 7) --------------------------------------------------

namespace {

Rgb pixel_at(const Fixture3D& f, u32 x, u32 y) {
    const usize i = (usize(y) * kW + x) * 4;
    return {f.pixels[i], f.pixels[i + 1], f.pixels[i + 2]};
}

void atmosphere_scene(Scene3D& s, f32 hours) {
    DirectionalLight sun;
    sun.direction = sun_direction_at(hours, 60.0f, 20.0f);
    sun.color = {1, 1, 1};
    sun.intensity = 3.0f;
    s.set_light(sun);
    Atmosphere a;
    a.enabled = true;
    s.set_atmosphere(a);
}

// Night: no sun, ambient or sky, so whatever the fog shows is light it scattered.
void night_scene(Scene3D& s) {
    DirectionalLight sun;
    sun.intensity = 0.0f;
    sun.ambient_sky = sun.ambient_ground = {0, 0, 0};
    sun.shadows = false;
    s.set_light(sun);
    Sky sky;
    sky.enabled = false;
    s.set_sky(sky);
    Fog fog;
    fog.color = {0, 0, 0};
    s.set_fog(fog);
}

Volumetrics dense_fog(f32 density = 0.12f) {
    Volumetrics v;
    v.enabled = true;
    v.density = density;
    v.height_falloff = 0.0f;
    v.max_distance = 24.0f;
    return v;
}

}  // namespace

TEST_CASE("render3d_gl: atmosphere and volumetrics off draw today's frame; a run in between and Low leave no trace") {
    Fixture3D f;
    const Forward3D& fw = f.renderer.forward_3d();
    f.render(0.02f);
    const Array<u8> off = f.pixels;
    CHECK_FALSE(fw.last_stats().atmosphere);
    CHECK_FALSE(fw.last_stats().volumetrics);
    f.render(0.02f, true, [](Scene3D& s) {
        Atmosphere a;
        a.enabled = true;
        s.set_atmosphere(a);
        s.set_volumetrics(dense_fog());
    });
    CHECK(fw.last_stats().atmosphere);
    CHECK(fw.last_stats().volumetrics);
    CHECK(fw.last_stats().froxels > 0);
    CHECK(differing_bytes(f.pixels, off) > 0);
    f.render(0.02f);
    CHECK(differing_bytes(f.pixels, off) == 0);
    // The Low tier turns the volumetric fog off: the exp² fog of the same scene.
    f.render(0.02f, true, [](Scene3D& s) {
        Volumetrics v = dense_fog();
        apply_quality(quality_settings(QualityTier::Low), v);
        s.set_volumetrics(v);
    });
    CHECK_FALSE(fw.last_stats().volumetrics);
    CHECK(differing_bytes(f.pixels, off) == 0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the atmosphere's sun goes orange at sunset; its sky is blue at noon") {
    Fixture3D f;
    f.pitch_deg = 5.0f;  // the top rows are sky
    f.render(0.0f, true, [](Scene3D& s) { atmosphere_scene(s, 12.0f); });
    const Rgb noon_ground = f.at_world({-2.0f, 0, 1.0f});
    const Rgb noon_sky = pixel_at(f, kW / 2, 2);
    f.render(0.0f, true, [](Scene3D& s) { atmosphere_scene(s, 17.6f); });
    const Rgb dusk_ground = f.at_world({-2.0f, 0, 1.0f});
    INFO("noon ground " << noon_ground.r << "," << noon_ground.g << "," << noon_ground.b << " sky " << noon_sky.r
                        << "," << noon_sky.g << "," << noon_sky.b << "; dusk ground " << dusk_ground.r << ","
                        << dusk_ground.g << "," << dusk_ground.b);
    CHECK(noon_sky.b > noon_sky.r + 10);
    CHECK(noon_ground.luma() > dusk_ground.luma());
    const f32 noon_warmth = f32(noon_ground.r + 1) / f32(noon_ground.b + 1);
    const f32 dusk_warmth = f32(dusk_ground.r + 1) / f32(dusk_ground.b + 1);
    CHECK(dusk_warmth > noon_warmth * 1.3f);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the atmosphere bakes once per sun position, and its IBL with it") {
    Fixture3D f;
    const Forward3D& fw = f.renderer.forward_3d();
    f32 hours = 10.0f;
    const auto scene = [&](Scene3D& s) {
        atmosphere_scene(s, hours);
        Sky sky;
        sky.ibl = true;  // the atmosphere is the IBL's source
        s.set_sky(sky);
    };
    f.render(0.0f, true, scene);
    CHECK(fw.last_stats().atmosphere_baked);
    CHECK(fw.last_stats().ibl_baked);
    f.render(0.0f, true, scene);
    CHECK(fw.last_stats().atmosphere);
    CHECK_FALSE(fw.last_stats().atmosphere_baked);
    CHECK_FALSE(fw.last_stats().ibl_baked);
    hours = 10.001f;  // well under rebake_degrees: the same quantized sun
    f.render(0.0f, true, scene);
    CHECK_FALSE(fw.last_stats().atmosphere_baked);
    hours = 11.0f;
    f.render(0.0f, true, scene);
    CHECK(fw.last_stats().atmosphere_baked);
    CHECK(fw.last_stats().ibl_baked);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: volumetric fog scatters forward: brighter looking into a low sun") {
    Fixture3D f;
    // The camera looks toward -z; a low sun in front of it (its light travels +z) or behind it.
    const auto scene = [](f32 g, f32 travel_z) {
        return [=](Scene3D& s) {
            DirectionalLight sun;
            sun.direction = math::normalize(vec3{0.0f, -0.25f, travel_z});
            sun.ambient_sky = sun.ambient_ground = {0.02f, 0.02f, 0.02f};
            s.set_light(sun);
            Volumetrics v = dense_fog(0.08f);
            v.anisotropy = g;
            s.set_volumetrics(v);
        };
    };
    f.render(0.0f, true, scene(0.7f, 1.0f));
    const int toward_forward = f.at_world({0, 0, -4.0f}).luma();
    f.render(0.0f, true, scene(-0.7f, 1.0f));
    const int toward_back = f.at_world({0, 0, -4.0f}).luma();
    f.render(0.0f, true, scene(0.7f, -1.0f));
    const int behind_forward = f.at_world({0, 0, -4.0f}).luma();
    f.render(0.0f, true, scene(-0.7f, -1.0f));
    const int behind_back = f.at_world({0, 0, -4.0f}).luma();
    INFO("toward sun g+ " << toward_forward << " g- " << toward_back << "; sun behind g+ " << behind_forward
                          << " g- " << behind_back);
    CHECK(toward_forward > toward_back + 8);
    CHECK(behind_back > behind_forward + 8);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a roof's shadow cuts a dark shaft out of the sunlit fog") {
    Fixture3D f;
    f.pitch_deg = 5.0f;  // the top rows see past the ground: nothing but fog in front of the clear
    // A slab high above the +x half, out of view: its shadow is a slanted column through the fog
    // the right half of the screen looks through. Without its shadow the fog there is sunlit.
    bool roof_shadow = true;
    const auto scene = [&](Scene3D& s) {
        night_scene(s);
        DirectionalLight sun;
        sun.direction = {-0.5f, -1.0f, -0.3f};
        sun.ambient_sky = sun.ambient_ground = {0, 0, 0};
        s.set_light(sun);
        Sky sky = constant_sky(0.0f, false);  // a black dome: the fog is applied over the sky pass
        sky.sun_intensity = 0.0f;
        s.set_sky(sky);
        MeshInstance roof;
        roof.mesh = 2;
        roof.world = math::translation(vec3{5.0f, 6.0f, 1.0f}) * math::scaling(vec3{0.5f, 1.0f, 0.6f});
        roof.cast_shadows = roof_shadow;
        s.add(roof);
        s.set_volumetrics(dense_fog(0.1f));
    };
    f.render(0.0f, true, scene);
    const Rgb shaft = pixel_at(f, kW * 3 / 4, 4), open = pixel_at(f, kW / 8, 4);
    roof_shadow = false;
    f.render(0.0f, true, scene);
    const Rgb shaft_lit = pixel_at(f, kW * 3 / 4, 4), open_lit = pixel_at(f, kW / 8, 4);
    INFO("under the roof " << shaft.luma() << " (" << shaft_lit.luma() << " unshadowed), open " << open.luma() << " ("
                           << open_lit.luma() << ")");
    CHECK(shaft_lit.luma() > shaft.luma() + 10);
    CHECK(std::abs(open_lit.luma() - open.luma()) <= 2);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a point light glows in the volumetric fog around it") {
    Fixture3D f;
    bool light = true;
    const auto scene = [&](Scene3D& s) {
        night_scene(s);
        if (light) {
            PointLight l;
            l.position = {2.0f, 1.3f, 2.0f};
            l.radius = 1.2f;  // short of the ground
            l.color = {1.0f, 0.5f, 0.2f};
            l.intensity = 2.0f;
            s.add_point_light(l);
        }
        s.set_volumetrics(dense_fog(0.15f));
    };
    f.render(0.0f, false, scene);
    const Rgb glow = f.at_world({2.0f, 1.3f, 2.0f});
    const Rgb away = f.at_world({-2.0f, 1.3f, 2.0f});
    light = false;
    f.render(0.0f, false, scene);
    const Rgb dark = f.at_world({2.0f, 1.3f, 2.0f});
    INFO("glow " << glow.r << "," << glow.g << "," << glow.b << " away " << away.luma() << " dark " << dark.luma());
    CHECK(glow.luma() > dark.luma() + 20);
    CHECK(glow.luma() > away.luma() + 15);
    CHECK(glow.r > glow.b);
    CHECK(f.device->validation_error_count() == 0);
}

// ---- post effects: auto exposure, color grading, depth of field, motion blur (M-VISUAL 8) --------

namespace {

struct ExposureReadback {
    u32 bins[Forward3D::kExposureBins];
    f32 exposure;
    f32 average;
    u32 frames;
    u32 pad;
};

ExposureReadback read_exposure(Fixture3D& f) {
    ExposureReadback e{};
    const rhi::BufferHandle b = f.renderer.forward_3d().exposure_buffer();
    REQUIRE(!b.is_null());
    REQUIRE(f.device->read_buffer(b, 0, Span<byte>(reinterpret_cast<byte*>(&e), sizeof(e))).has_value());
    return e;
}

// Sum of squared steps in luma to the left and above over the region: how much sharp edge it shows
// (a blur spreads a step over several smaller ones, which square to less).
u64 edge_energy(const Array<u8>& px, u32 y0, u32 y1, u32 x0 = 0, u32 x1 = kW) {
    u64 sum = 0;
    for (u32 y = y0; y < y1; ++y)
        for (u32 x = x0 + 1; x < x1; ++x) {
            const usize a = (usize(y) * kW + x) * 4, b = a - 4;
            const int la = (px[a] * 2 + px[a + 1] * 5 + px[a + 2]) / 8;
            const int lb = (px[b] * 2 + px[b + 1] * 5 + px[b + 2]) / 8;
            sum += u64((la - lb) * (la - lb));
            if (y > y0) {
                const usize c = a - usize(kW) * 4;
                const int lc = (px[c] * 2 + px[c + 1] * 5 + px[c + 2]) / 8;
                sum += u64((la - lc) * (la - lc));
            }
        }
    return sum;
}

u32 upload_lut(Fixture3D& f, bool invert) {
    Array<u8> lut;
    make_identity_lut(lut);
    if (invert)
        for (usize i = 0; i < lut.size(); i += 4)
            for (u32 c = 0; c < 3; ++c) lut[i + c] = static_cast<u8>(255 - lut[i + c]);
    const u32 key = f.renderer.textures().allocate_key();
    const TextureMipUpload mip{kLutSize * kLutSize, kLutSize, std::as_bytes(lut.span())};
    TextureUpload up;
    up.format = rhi::TextureFormat::RGBA8_UNorm;
    up.mips = Span<const TextureMipUpload>(&mip, 1);
    REQUIRE(f.renderer.textures().upload(key, 1, up).has_value());
    return key;
}

}  // namespace

TEST_CASE("render3d_gl: post effects off draw today's frame; a run with all of them in between leaves no trace") {
    Fixture3D f;
    const u32 lut = upload_lut(f, true);
    const Forward3D& fw = f.renderer.forward_3d();
    f.render();
    const Array<u8> off = f.pixels;
    math::mat4 prev = math::translation({0.3f, 0.0f, 0.0f});
    for (int i = 0; i < 2; ++i)
        f.render(0.0f, true, [&](Scene3D& s) {
            PostFx p;
            p.auto_exposure.enabled = true;
            p.lut_key = lut;
            p.dof.enabled = true;
            p.motion_blur.enabled = true;
            s.set_post(p);
            MeshInstance m;
            m.mesh = 1;
            m.world = math::translation({1.5f, 0.0f, 1.0f});
            m.prev_world = &prev;
            s.add(m);
        });
    CHECK(fw.last_stats().auto_exposure);
    CHECK(fw.last_stats().color_grade);
    CHECK(fw.last_stats().dof);
    CHECK(fw.last_stats().motion_blur);
    CHECK(differing_bytes(f.pixels, off) > 0);
    f.render();
    CHECK_FALSE(fw.last_stats().auto_exposure);
    CHECK_FALSE(fw.last_stats().dof);
    CHECK(differing_bytes(f.pixels, off) == 0);
    // The Low tier drops depth of field and motion blur.
    f.render(0.0f, true, [](Scene3D& s) {
        PostFx p;
        p.dof.enabled = true;
        p.motion_blur.enabled = true;
        ShadowSettings ss = s.shadow_settings();
        apply_quality(quality_settings(QualityTier::Low), ss, p);
        p.bloom = p.fxaa = true;  // the fixture's defaults: only the dropped effects differ
        s.set_post(p);
    });
    CHECK_FALSE(fw.last_stats().dof);
    CHECK_FALSE(fw.last_stats().motion_blur);
    CHECK(differing_bytes(f.pixels, off) == 0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: auto exposure brings a dim and a bright sun to the same brightness, adapting over time") {
    Fixture3D f;
    f32 sun_scale = 0.1f, dt = 1.0f / 60.0f;
    bool on = true;
    const auto scene = [&](Scene3D& s) {
        DirectionalLight sun;
        sun.direction = {-0.5f, -1.0f, -0.3f};
        sun.intensity = 3.0f * sun_scale;
        sun.ambient_sky = vec3{0.35f, 0.42f, 0.55f} * sun_scale;
        sun.ambient_ground = vec3{0.20f, 0.18f, 0.16f} * sun_scale;
        s.set_light(sun);
        Sky sky;
        sky.zenith *= sun_scale;
        sky.horizon *= sun_scale;
        sky.ground *= sun_scale;
        s.set_sky(sky);
        PostFx p;
        p.auto_exposure.enabled = on;
        p.auto_exposure.dt = dt;
        s.set_post(p);
    };
    const vec3 ground{-2.0f, 0.0f, 1.0f};
    on = false;
    f.render(0.0f, true, scene);
    const int dim_fixed = f.at_world(ground).luma();
    sun_scale = 1.0f;
    f.render(0.0f, true, scene);
    const int bright_fixed = f.at_world(ground).luma();
    on = true;
    sun_scale = 0.1f;
    f.render(0.0f, true, scene);  // the first frame snaps
    const ExposureReadback dim = read_exposure(f);
    const int dim_auto = f.at_world(ground).luma();
    sun_scale = 1.0f;
    f.render(0.0f, true, scene);  // one 60 Hz frame toward the bright target
    const ExposureReadback step = read_exposure(f);
    dt = 1.0f;
    for (int i = 0; i < 4; ++i) f.render(0.0f, true, scene);
    const ExposureReadback bright = read_exposure(f);
    const int bright_auto = f.at_world(ground).luma();
    INFO("fixed: dim " << dim_fixed << " bright " << bright_fixed << "; auto: dim " << dim_auto << " (x" << dim.exposure
                       << ", avg " << dim.average << ") bright " << bright_auto << " (x" << bright.exposure << ", avg "
                       << bright.average << "), one frame after the switch x" << step.exposure);
    CHECK(bright_fixed > dim_fixed + 60);
    CHECK(std::abs(bright_auto - dim_auto) <= 6);
    CHECK(dim.exposure > 4.0f * bright.exposure);
    CHECK(bright.average > 5.0f * dim.average);
    // Adaptation: a frame later the exposure has moved only part of the way.
    CHECK(step.exposure < dim.exposure);
    CHECK(step.exposure > 2.0f * bright.exposure);
    for (u32 i = 0; i < Forward3D::kExposureBins; ++i) CHECK(bright.bins[i] == 0);  // emptied for the next frame
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a color-grade LUT maps the tonemapped frame: identity keeps it, an inverted one flips it") {
    Fixture3D f;
    const u32 ident = upload_lut(f, false), inverted = upload_lut(f, true);
    const auto graded = [&](u32 key, f32 strength) {
        return [=](Scene3D& s) {
            PostFx p;
            p.lut_key = key;
            p.lut_strength = strength;
            s.set_post(p);
        };
    };
    const vec3 box_face{0.0f, 0.5f, 0.5f}, ground{-2.0f, 0.0f, 1.0f};
    f.render();
    const Rgb box0 = f.at_world(box_face), ground0 = f.at_world(ground);
    f.render(0.0f, true, graded(ident, 1.0f));
    const Rgb box1 = f.at_world(box_face), ground1 = f.at_world(ground);
    f.render(0.0f, true, graded(inverted, 1.0f));
    const Rgb box2 = f.at_world(box_face), ground2 = f.at_world(ground);
    f.render(0.0f, true, graded(inverted, 0.5f));
    const Rgb ground_half = f.at_world(ground);
    INFO("box " << box0.r << "," << box0.g << "," << box0.b << " identity " << box1.r << "," << box1.g << "," << box1.b
                << " inverted " << box2.r << "," << box2.g << "," << box2.b << "; ground " << ground0.luma() << " / "
                << ground1.luma() << " / " << ground2.luma() << ", half " << ground_half.luma());
    CHECK(std::abs(box1.r - box0.r) <= 3);
    CHECK(std::abs(box1.g - box0.g) <= 3);
    CHECK(std::abs(ground1.luma() - ground0.luma()) <= 3);
    CHECK(std::abs(box2.r - (255 - box0.r)) <= 6);
    CHECK(std::abs(box2.b - (255 - box0.b)) <= 6);
    CHECK(std::abs(ground2.luma() - (255 - ground0.luma())) <= 6);
    CHECK(std::abs(ground_half.luma() - 128) <= 8);
    CHECK(f.renderer.forward_3d().last_stats().color_grade);
    // A key the store does not hold (the kit's ~0u "no texture") leaves the frame ungraded.
    f.render(0.0f, true, graded(~0u, 1.0f));
    CHECK_FALSE(f.renderer.forward_3d().last_stats().color_grade);
    CHECK(f.at_world(ground).luma() == ground0.luma());
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("M-ACCESS: the tonemap pass applies the color-vision correction after the grade") {
    Fixture3D f;
    ColorVision cv = ColorVision::None;
    f32 strength = 1.0f;
    const auto scene = [&](Scene3D& s) {
        PostFx p;
        p.fxaa = false;
        p.color_vision = cv;
        p.color_vision_strength = strength;
        s.set_post(p);
    };
    const auto decode = [](int v) {
        const f32 c = static_cast<f32>(v) / 255.0f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    const auto encode = [](f32 l) {
        const f32 c = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
        return static_cast<int>(std::lround(c * 255.0f));
    };
    const vec3 box_face{0.0f, 0.5f, 0.5f};
    f.render(0.0f, true, scene);
    const Rgb box0 = f.at_world(box_face);
    CHECK_FALSE(f.renderer.forward_3d().last_stats().color_vision);
    for (ColorVision mode : {ColorVision::Protanopia, ColorVision::Deuteranopia, ColorVision::Tritanopia}) {
        cv = mode;
        f.render(0.0f, true, scene);
        const Rgb got = f.at_world(box_face);
        const vec3 want = correct_color_vision(mode, 1.0f, {decode(box0.r), decode(box0.g), decode(box0.b)});
        INFO(color_vision_name(mode) << ": box " << box0.r << "," << box0.g << "," << box0.b << " -> " << got.r << "," << got.g
                                     << "," << got.b << " want " << encode(want.x) << "," << encode(want.y) << "," << encode(want.z));
        CHECK(std::abs(got.r - encode(want.x)) <= 4);
        CHECK(std::abs(got.g - encode(want.y)) <= 4);
        CHECK(std::abs(got.b - encode(want.z)) <= 4);
        CHECK(f.renderer.forward_3d().last_stats().color_vision);
    }
    strength = 0.0f;  // strength 0: off
    f.render(0.0f, true, scene);
    CHECK_FALSE(f.renderer.forward_3d().last_stats().color_vision);
    CHECK(std::abs(f.at_world(box_face).r - box0.r) <= 1);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: depth of field blurs what lies away from the focus and keeps the focus sharp") {
    Fixture3D f;
    f.pitch_deg = 12.0f;  // the box in front of the ground's far edge against the sky
    f32 focus = 7.0f;
    bool on = false;
    const auto scene = [&](Scene3D& s) {
        PostFx p;
        p.fxaa = false;
        p.bloom = false;
        p.dof.enabled = on;
        p.dof.focus_distance = focus;
        p.dof.focus_range = 0.8f;
        p.dof.blur_range = 4.0f;
        p.dof.max_radius = 0.03f;
        s.set_post(p);
    };
    const auto box_edges = [&] { return edge_energy(f.pixels, kH / 3, kH * 2 / 3, kW / 3, kW * 2 / 3); };
    const auto far_edges = [&] { return edge_energy(f.pixels, kH / 3, kH / 2, 0, kW / 3); };  // the ground's far edge
    f.render(0.0f, true, scene);
    const u64 box_sharp = box_edges(), far_sharp = far_edges();
    const Rgb centre_sharp = f.at_world({0.0f, 0.5f, 0.5f});
    on = true;
    f.render(0.0f, true, scene);  // focused on the box: the far edge blurs
    const u64 box_near = box_edges(), far_near = far_edges();
    const Rgb centre_near = f.at_world({0.0f, 0.5f, 0.5f});
    focus = 30.0f;  // focused far: the box blurs
    f.render(0.0f, true, scene);
    const u64 box_far = box_edges();
    INFO("box edges: sharp " << box_sharp << ", focus on it " << box_near << ", focus far " << box_far << "; far edges: sharp "
                             << far_sharp << ", focus on the box " << far_near);
    CHECK(far_near * 10 < far_sharp * 8);
    CHECK(box_near * 10 > box_sharp * 8);
    CHECK(box_far * 10 < box_sharp * 7);
    CHECK(std::abs(centre_near.luma() - centre_sharp.luma()) <= 2);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: motion blur smears a moving box along its motion and leaves a still frame alone") {
    Fixture3D f(1);
    upload_slider(f);
    bool blur = false, moving = true;
    math::mat4 prev = math::translation({-0.6f, 0.0f, 1.5f});
    const math::mat4 world = math::translation({0.0f, 0.0f, 1.5f});
    const auto scene = [&](Scene3D& s) {
        PostFx p;
        p.fxaa = false;
        p.bloom = false;
        p.motion_blur.enabled = blur;
        p.motion_blur.intensity = 1.0f;
        s.set_post(p);
        MeshInstance m;
        m.mesh = 50;
        m.world = world;
        if (moving) m.prev_world = &prev;
        s.add(m);
    };
    f.render(0.0f, true, scene);
    const Array<u8> still = f.pixels;
    blur = true;
    f.render(0.0f, true, scene);
    CHECK(f.renderer.forward_3d().last_stats().motion_blur);
    // Along the motion (x) the box's edges soften.
    const u64 sharp = edge_energy(still, kH / 3, kH * 2 / 3), blurred = edge_energy(f.pixels, kH / 3, kH * 2 / 3);
    const f64 changed = mean_abs_diff(f.pixels, still);
    // A still box (no previous transform) with a still camera: nothing moves, nothing blurs.
    moving = false;
    f.render(0.0f, true, scene);  // the previous view-proj is now this one
    f.render(0.0f, true, scene);
    blur = false;
    const Array<u8> blurred_still = f.pixels;
    f.render(0.0f, true, scene);
    INFO("horizontal edges: sharp " << sharp << ", blurred " << blurred << "; mean |diff| " << changed);
    CHECK(blurred * 10 < sharp * 9);
    CHECK(changed > 0.2);
    CHECK(differing_bytes(f.pixels, blurred_still) == 0);
    CHECK(f.device->validation_error_count() == 0);
}
