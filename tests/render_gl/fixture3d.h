#pragma once
// The 3D test fixture shared by test_render3d_gl.cpp (pixel probes) and test_golden.cpp (golden images):
// a window + device + Renderer with a box on a ground slab, rendered into a 160x120 offscreen target and
// read back, plus the scene helpers of the M-VISUAL 2 cases.

#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>
#include <functional>

#include "../render/mesh_fixture.h"
#include "mce/core.h"
#include "mce/platform/platform.h"
#include "mce/platform/window.h"
#include "mce/render/renderer.h"
#include "mce/rhi/default_device.h"

namespace mce::render_test {

using namespace mce::render;
using math::vec3;


inline constexpr u32 kW = 160, kH = 120;

struct Rgb {
    int r, g, b;
    int luma() const { return (r * 2 + g * 5 + b) / 8; }
};

struct Fixture3D {
    explicit Fixture3D(u32 msaa = 4, bool gpu_clusters = true, bool ibl = true, f32 render_scale = 1.0f) {
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
        fd.render_scale = render_scale;
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


// Sun off: the ambient is all that lights the frame.
inline void no_sun(Scene3D& s) {
    DirectionalLight sun;
    sun.intensity = 0.0f;
    sun.shadows = false;
    s.set_light(sun);
}

inline Sky constant_sky(f32 c, bool ibl) {
    Sky sky;
    sky.zenith = sky.horizon = sky.ground = {c, c, c};
    sky.ibl = ibl;
    return sky;
}


// Meshes 7 and 8: a smooth metal floor just above the ground slab and a red emissive wall behind
// the box. The dim sky's reflection shows no wall, a probe's box-projected capture does.
inline void upload_probe_room(Fixture3D& f) {
    mesh_fixture::Builder floor;
    floor.add_node(-1);
    mesh_fixture::Box fb;
    fb.center = {0, 0.02f, 0};
    fb.half = {6, 0.03f, 6};
    floor.add_box(fb);
    floor.submeshes.back().material.metallic = 1.0f;
    floor.submeshes.back().material.roughness = 0.1f;
    REQUIRE(f.renderer.meshes().upload(7, 1, floor.build().span()).has_value());
    mesh_fixture::Builder wall;
    wall.add_node(-1);
    mesh_fixture::Box wb;
    wb.center = {0, 1.5f, -1.2f};
    wb.half = {3, 1.5f, 0.1f};
    wb.color[0] = wb.color[1] = wb.color[2] = 0.0f;
    wall.add_box(wb);
    wall.submeshes.back().material.emissive[0] = 3.0f;
    REQUIRE(f.renderer.meshes().upload(8, 1, wall.build().span()).has_value());
}

inline ReflectionProbe room_probe() {
    ReflectionProbe p;
    p.position = {1.5f, 0.6f, 1.0f};
    p.bounds = {{-3, -0.5f, -1.1f}, {3, 3, 3}};  // floor inside by more than the blend distance
    p.blend_distance = 0.5f;
    return p;
}

// The irradiance volume of the GI cases (M-VISUAL 6): 4 × 3 × 4 probes around the box, none inside
// it or inside the probe room's wall (z -1.3 .. -1.1).
inline GiVolume room_gi() {
    GiVolume g;
    g.enabled = true;
    g.bounds = {{-3, 0.2f, -3}, {3, 2.2f, 3}};
    g.counts[0] = 4;
    g.counts[1] = 3;
    g.counts[2] = 4;
    g.probes_per_frame = 8;
    return g;
}

// The probe room's red emissive wall behind the box under a dim constant sky, no sun: what the
// ground in front of the wall receives is the wall's light, bounced through the volume.
inline void gi_wall_scene(Scene3D& s, bool gi) {
    no_sun(s);
    s.set_sky(constant_sky(0.05f, false));
    MeshInstance m;
    m.mesh = 8;
    s.add(m);
    if (gi) s.set_gi_volume(room_gi());
}

}  // namespace mce::render_test
