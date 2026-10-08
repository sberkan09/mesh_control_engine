// The MVP 3D frame against a real GL 4.5 driver (CI: Xvfb + Mesa llvmpipe): a lit box on a ground
// slab rendered through shadow -> forward HDR (MSAA) -> resolve -> tonemap into an offscreen
// target, read back and probed: lit faces, the box's shadow on the ground, fog, MSAA on/off
// agreement, the RHI additions (multisample resolve, depth-compare sampling, snorm/unorm16
// vertex streams) and zero GL validation errors.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>

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

    void render(f32 fog_density = 0.0f, bool shadows = true) {
        camera = orbit_camera({0, 0.5f, 0}, 0.0f, math::radians(35.0f), 7.0f);
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
        MeshInstance ground;
        ground.mesh = 2;
        scene.add(ground);
        MeshInstance box;
        box.mesh = 1;
        scene.add(box);
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
    CHECK(f.renderer.forward_3d().last_stats().shadow_draws == 2);
    CHECK(f.renderer.forward_3d().last_stats().main_draws == 2);
    CHECK(f.gpu.passes == 3);  // shadow, forward, tonemap
    CHECK(f.device->validation_error_count() == 0);

    // Shadows off: the same ground point is lit.
    f.render(0.0f, false);
    CHECK(f.at_world({-0.8f, 0, -0.3f}).luma() > shadow.luma() + 30);
    CHECK(f.gpu.passes == 2);
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
