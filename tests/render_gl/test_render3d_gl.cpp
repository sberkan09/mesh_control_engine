// The 3D frame against a real GL 4.5 driver (CI: Xvfb + Mesa llvmpipe): a lit box on a ground
// slab rendered through cascaded shadows -> Forward+ PBR HDR (MSAA) + sky -> resolve -> bloom ->
// tonemap -> FXAA into an offscreen target, read back and probed: lit faces, the box's shadow on
// the ground, fog, MSAA on/off agreement, metal vs dielectric, clustered point lights, the sky,
// the post toggles, GPU timestamps, the RHI additions (multisample resolve, layered depth targets,
// depth-compare array sampling, storage data, snorm/unorm16 vertex streams) and zero GL
// validation errors.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <functional>

#include "../render/mesh_fixture.h"
#include "mce/core.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/renderer.h"
#include "mce/rhi_gl/gl_device.h"

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
    explicit Fixture3D(u32 msaa = 4) {
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
        auto d = rhi::gl::create_device(*window);
        REQUIRE(d.has_value());
        device = std::move(d).value();
        REQUIRE(renderer.init(*device).has_value());
        Forward3DDesc fd;
        fd.msaa = msaa;
        fd.shadow_size = 1024;
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
            l.position = {-5.5f + 0.04f * f32(i % 16), 0.3f, -5.5f + 0.04f * f32(i / 16)};
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
    CHECK(st.light_indices > 0);

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
