// The 3D frame against a real GL 4.5 driver (CI: Xvfb + Mesa llvmpipe): a lit box on a ground
// slab rendered through cascaded shadows -> Forward+ PBR HDR (MSAA) + sky -> resolve -> bloom ->
// tonemap -> FXAA into an offscreen target, read back and probed: lit faces, the box's shadow on
// the ground, fog, MSAA on/off agreement, metal vs dielectric, clustered point lights, the sky,
// the post toggles, GPU timestamps, the GPU light assignment against the CPU one, the RHI additions (multisample resolve, layered depth targets,
// depth-compare array sampling, storage data, snorm/unorm16 vertex streams) and zero GL
// validation errors.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <utility>

#include "../render/mesh_fixture.h"
#include "mce/core.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/renderer.h"
#include "mce/rhi/default_device.h"

using namespace mce;
using namespace mce::render;
using math::vec3;

namespace {

constexpr u32 kW = 160, kH = 120;

struct Rgb {
    int r, g, b;
    int luma() const { return (r * 2 + g * 5 + b) / 8; }
};

struct Fixture3D {
    explicit Fixture3D(u32 msaa = 4, bool gpu_clusters = true, bool ibl = true) {
        REQUIRE(platform::init().has_value());
        platform::WindowDesc wd;
        wd.title = "test_render3d_gl";
        wd.width = 64;
        wd.height = 64;
        wd.resizable = false;
        wd.high_dpi = false;
        auto w = platform::Window::create(wd);
        REQUIRE(w.has_value());
        window = std::move(w).value();
        auto d = rhi::create_default_device(*window);
        REQUIRE(d.has_value());
        device = std::move(d).value();
        REQUIRE(renderer.init(*device).has_value());
        Forward3DDesc fd;
        fd.msaa = msaa;
        fd.shadow_size = 1024;
        fd.gpu_light_clusters = gpu_clusters;
        fd.ibl = ibl;
        REQUIRE(renderer.init_3d(fd).has_value());
        rhi::TextureDesc td;
        td.size = {kW, kH};
        td.usage = rhi::TextureUsage::SampledColorTarget;
        td.debug_name = "test.target3d";
        target = device->create_texture(td);
        pixels.resize(usize(kW) * kH * 4);

        // Mesh 1: a 1 m box standing on the origin. Mesh 2: a 12 m × 0.2 m × 12 m ground slab.
        mesh_fixture::Builder box;
        box.add_node(-1);
        mesh_fixture::Box b;
        b.center = {0, 0.5f, 0};
        b.half = {0.5f, 0.5f, 0.5f};
        b.color[0] = 0.9f;
        b.color[1] = 0.3f;
        b.color[2] = 0.2f;
        box.add_box(b);
        REQUIRE(renderer.meshes().upload(1, 1, box.build().span()).has_value());
        mesh_fixture::Builder ground;
        ground.add_node(-1);
        mesh_fixture::Box g;
        g.center = {0, -0.1f, 0};
        g.half = {6, 0.1f, 6};
        g.color[0] = g.color[1] = g.color[2] = 0.7f;
        ground.add_box(g);
        REQUIRE(renderer.meshes().upload(2, 1, ground.build().span()).has_value());
    }
    ~Fixture3D() {
        device->destroy_texture(target);
        renderer.shutdown();
        device.reset();
        window.reset();
        platform::shutdown();
    }

    // `setup` runs after the defaults (sun, fog, ground, box) and before build().
    void render(f32 fog_density = 0.0f, bool shadows = true, const std::function<void(Scene3D&)>& setup = {}) {
        camera = orbit_camera({0, 0.5f, 0}, 0.0f, math::radians(pitch_deg), 7.0f);
        device->begin_frame(frame++);
        Scene3D scene(renderer.meshes(), mem::heap());
        scene.begin(camera, {kW, kH});
        DirectionalLight sun;
        sun.direction = {-0.5f, -1.0f, -0.3f};
        sun.shadows = shadows;
        scene.set_light(sun);
        Fog fog;
        fog.density = fog_density;
        scene.set_fog(fog);
        ShadowSettings ss;
        ss.resolution = 1024;  // = Forward3DDesc::shadow_size below
        scene.set_shadows(ss);
        MeshInstance ground;
        ground.mesh = 2;
        scene.add(ground);
        MeshInstance box;
        box.mesh = 1;
        scene.add(box);
        if (setup) setup(scene);
        scene.build();
        view = scene.view();
        rhi::CommandBuffer cmd;
        renderer.record_3d(cmd, scene, target);
        cpu_assigned = scene.cluster_setup().assigned();
        device->submit(cmd);
        REQUIRE(device->read_pixels(target, rhi::Rect2D{0, 0, kW, kH}, pixels.span()).has_value());
        device->end_frame();
        gpu = device->last_frame_stats();
        if (const char* dump = std::getenv("MCE_TEST_DUMP_3D")) {  // eyeballing aid: a PPM of the last frame
            if (std::FILE* fp = std::fopen(dump, "wb")) {
                std::fprintf(fp, "P6\n%u %u\n255\n", kW, kH);
                for (usize i = 0; i < pixels.size(); i += 4) std::fwrite(&pixels[i], 1, 3, fp);
                std::fclose(fp);
            }
        }
    }

    // Pixel showing world point p (read_pixels is top-row first).
    Rgb at_world(vec3 p) const {
        const math::vec4 c = view.view_proj * math::vec4(p, 1.0f);
        const f32 x = (c.x / c.w * 0.5f + 0.5f) * kW, y = (0.5f - c.y / c.w * 0.5f) * kH;
        REQUIRE(x >= 0);
        REQUIRE(y >= 0);
        REQUIRE(x < kW);
        REQUIRE(y < kH);
        const usize i = (usize(y) * kW + usize(x)) * 4;
        return {pixels[i], pixels[i + 1], pixels[i + 2]};
    }

    mem::UniquePtr<platform::Window> window;
    mem::UniquePtr<rhi::RenderDevice> device;
    Renderer renderer;
    rhi::TextureHandle target;
    Array<u8> pixels;
    f32 pitch_deg = 35.0f;
    Camera3D camera;
    View3D view;
    rhi::FrameStats gpu;
    bool cpu_assigned = false;  // the CPU light assignment ran for the last frame
    u64 frame = 0;
};

}  // namespace

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
    CHECK(t.shadow >= 0.0);
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

namespace {

// A failed REQUIRE records but does not leave the test case here (no exceptions): bail out.
#define MCE_REQUIRE_OR_RETURN(...)                  \
    do {                                            \
        const bool mce_req_ok_ = (__VA_ARGS__);     \
        REQUIRE_MESSAGE(mce_req_ok_, #__VA_ARGS__); \
        if (!mce_req_ok_) return;                   \
    } while (0)

// Sun off: the ambient is all that lights the frame.
void no_sun(Scene3D& s) {
    DirectionalLight sun;
    sun.intensity = 0.0f;
    sun.shadows = false;
    s.set_light(sun);
}

Sky constant_sky(f32 c, bool ibl) {
    Sky sky;
    sky.zenith = sky.horizon = sky.ground = {c, c, c};
    sky.ibl = ibl;
    return sky;
}

}  // namespace

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

