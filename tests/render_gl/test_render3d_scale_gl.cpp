// M-VISUAL 9 (scale) against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend:
// instance batches culled on the GPU draw the CPU path's frame, the depth pyramid drops hidden
// instances without changing the picture, batches cast shadows and sway in the wind, projected decals
// recolor what they cover, GPU particles spawn and glow, and none of it leaves a trace when off.

#include <doctest/doctest.h>

#include "fixture3d.h"
#include "mce/render/renderer.h"

using namespace mce;
using namespace mce::render;
using math::vec3;
using namespace mce::render_test;

namespace {

usize differing(const Array<u8>& a, const Array<u8>& b) {
    usize n = 0;
    for (usize i = 0; i < a.size(); ++i) n += a[i] != b[i];
    return n;
}

// Small boxes (mesh 1 scaled by 0.3) on a grid around the fixture's box, plus `far` of them well
// behind the camera, which every view culls.
Array<math::mat4> box_grid(u32 far = 0) {
    Array<math::mat4> out(mem::heap());
    for (int x = -3; x <= 3; ++x) {
        for (int z = -3; z <= 3; ++z) {
            if (x >= -1 && x <= 1 && z >= -1 && z <= 1) continue;  // clear of the fixture's box
            out.push_back(math::translation({0.8f * f32(x), 0.0f, 0.8f * f32(z)}) * math::scaling({0.3f, 0.3f, 0.3f}));
        }
    }
    for (u32 i = 0; i < far; ++i) out.push_back(math::translation({f32(i), 0.0f, 40.0f}) * math::scaling({0.3f, 0.3f, 0.3f}));
    return out;
}

InstanceBatch grid_batch(Span<const math::mat4> transforms, u32 version = 1) {
    InstanceBatch b;
    b.id = 7;
    b.version = version;
    b.mesh = 1;
    b.transforms = transforms;
    b.tint = {0.3f, 0.6f, 1.0f, 1.0f};
    return b;
}

InstanceCulling culling(bool gpu, bool occlusion) {
    InstanceCulling c;
    c.gpu = gpu;
    c.occlusion = occlusion;
    return c;
}

// The instances the GPU culling kept for (view, batch 0, level 0, submesh 0) last frame.
u32 gpu_visible(Fixture3D& f, u32 view) {
    const Forward3D& fw = f.renderer.forward_3d();
    const u32 c = fw.batch_command(view, 0, 0, 0);
    REQUIRE(c != ~0u);
    rhi::DrawIndexedIndirectCommand cmd{};
    REQUIRE(f.device
                ->read_buffer(fw.batch_commands(), u64(c) * sizeof(cmd), Span<byte>(reinterpret_cast<byte*>(&cmd), sizeof(cmd)))
                .has_value());
    return cmd.instance_count;
}

}  // namespace

TEST_CASE("render3d_scale: batches, decals and particles off draw today's frame; a run in between leaves no trace") {
    Fixture3D f;
    const Forward3D& fw = f.renderer.forward_3d();
    f.render();
    const Array<u8> off = f.pixels;
    CHECK(fw.last_stats().batches == 0);
    const Array<math::mat4> grid = box_grid();
    for (int i = 0; i < 2; ++i)
        f.render(0.0f, true, [&](Scene3D& s) {
            s.set_time(0.1f * f32(i + 1), 0.1f);
            s.add_batch(grid_batch(grid.span()));
            Decal d;
            d.world = math::translation({2.0f, 0.0f, 2.0f}) * math::scaling({1.0f, 1.0f, 1.0f});
            d.color = {0.0f, 0.0f, 1.0f, 1.0f};
            s.add_decal(d);
            ParticleEmitter e;
            e.id = 3;
            e.position = {-1.5f, 1.0f, 1.0f};
            e.burst = 64;
            s.add_particle_emitter(e);
        });
    CHECK(fw.last_stats().batches == 1);
    CHECK(fw.last_stats().decals == 1);
    CHECK(fw.last_stats().particle_emitters == 1);
    CHECK(differing(f.pixels, off) > 0);
    f.render();
    CHECK(fw.last_stats().batches == 0);
    CHECK(fw.last_stats().decals == 0);
    CHECK(fw.last_stats().particle_emitters == 0);
    CHECK(differing(f.pixels, off) == 0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_scale: GPU-culled batches draw the CPU path's frame; both drop what lies out of view") {
    Fixture3D f;
    const Forward3D& fw = f.renderer.forward_3d();
    const Array<math::mat4> grid = box_grid(5);
    const u32 in_view = static_cast<u32>(grid.size()) - 5;
    f.render(0.0f, true, [&](Scene3D& s) {
        s.set_instance_culling(culling(false, false));
        s.add_batch(grid_batch(grid.span()));
    });
    const Array<u8> cpu = f.pixels;
    CHECK_FALSE(fw.last_stats().batches_gpu);
    CHECK(fw.last_stats().batch_views == 1 + fw.last_stats().cascades);
    CHECK(fw.last_stats().batch_visible == in_view);  // the grid is in view, the far five behind the camera
    CHECK(fw.last_stats().batch_draws > 0);
    if (!f.device->caps().compute) return;
    f.render(0.0f, true, [&](Scene3D& s) {
        s.set_instance_culling(culling(true, false));
        s.add_batch(grid_batch(grid.span()));
    });
    CHECK(fw.last_stats().batches_gpu);
    CHECK_FALSE(fw.last_stats().occlusion);
    CHECK(gpu_visible(f, 0) == in_view);
    CHECK(differing(f.pixels, cpu) == 0);
    // The batch's shadows: the grid darkens the ground between its boxes (shadows off: brighter).
    const Array<u8> shadowed = f.pixels;
    f.render(0.0f, true, [&](Scene3D& s) {
        s.set_instance_culling(culling(true, false));
        InstanceBatch b = grid_batch(grid.span());
        b.cast_shadows = false;
        s.add_batch(b);
    });
    CHECK(fw.last_stats().batch_views == 1);
    CHECK(differing(f.pixels, shadowed) > 0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_scale: occlusion culling drops the instances behind a wall and draws the same frame") {
    Fixture3D f;
    if (!f.device->caps().compute) return;
    upload_probe_room(f);  // mesh 8: a 6 m × 3 m wall at z = -1.2
    const Forward3D& fw = f.renderer.forward_3d();
    Array<math::mat4> rows(mem::heap());
    for (int x = -4; x <= 4; ++x) {
        rows.push_back(math::translation({0.5f * f32(x), 0.0f, -3.0f}) * math::scaling({0.3f, 0.3f, 0.3f}));  // behind the wall
        rows.push_back(math::translation({0.5f * f32(x), 0.0f, 2.0f}) * math::scaling({0.3f, 0.3f, 0.3f}));   // in front
    }
    auto scene = [&](bool occlusion) {
        return [&, occlusion](Scene3D& s) {
            s.set_instance_culling(culling(true, occlusion));
            MeshInstance wall;
            wall.mesh = 8;
            s.add(wall);
            s.add_batch(grid_batch(rows.span()));
        };
    };
    f.render(0.0f, true, scene(false));
    const Array<u8> all = f.pixels;
    const u32 unoccluded = gpu_visible(f, 0);
    const u32 cascade0 = gpu_visible(f, 1);
    f.render(0.0f, true, scene(true));
    CHECK(fw.last_stats().occlusion);
    CHECK(fw.last_stats().occluder_draws >= 2);  // the wall, the ground and the fixture's box
    const u32 occluded = gpu_visible(f, 0);
    INFO("visible " << unoccluded << " -> " << occluded);
    CHECK(unoccluded == 18);
    CHECK(occluded >= 9);   // nothing in front of the wall is dropped
    CHECK(occluded < 18);   // ... and some of what is behind it is
    CHECK(differing(f.pixels, all) == 0);
    // The cascades see past the wall: the hidden instances still cast their shadows.
    CHECK(gpu_visible(f, 1) == cascade0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_scale: wind sways a batch over time; without wind the clock changes nothing") {
    Fixture3D f;
    const Array<math::mat4> grid = box_grid();
    auto at = [&](f32 t, f32 amplitude) {
        f.render(0.0f, true, [&, t, amplitude](Scene3D& s) {
            s.set_time(t, 0.016f);
            InstanceBatch b = grid_batch(grid.span());
            b.wind_amplitude = amplitude;
            b.wind_frequency = 1.0f;
            s.add_batch(b);
        });
        return f.pixels;
    };
    const Array<u8> still0 = at(0.0f, 0.0f), still1 = at(0.37f, 0.0f);
    CHECK(differing(still0, still1) == 0);
    const Array<u8> wind0 = at(0.0f, 0.4f), wind1 = at(0.37f, 0.4f);
    CHECK(differing(wind0, wind1) > 0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_scale: a decal recolors the ground it covers and nothing outside its box") {
    Fixture3D f;
    const Forward3D& fw = f.renderer.forward_3d();
    f.render();
    const Rgb inside0 = f.at_world({2.0f, 0.0f, 2.0f}), outside0 = f.at_world({-2.0f, 0.0f, 2.0f});
    f.render(0.0f, true, [](Scene3D& s) {
        Decal d;
        d.world = math::translation({2.0f, 0.0f, 2.0f}) * math::scaling({1.2f, 0.6f, 1.2f});
        d.color = {0.05f, 0.1f, 1.0f, 1.0f};
        s.add_decal(d);
        Decal culled;  // well behind the camera
        culled.world = math::translation({0.0f, 0.0f, 40.0f});
        s.add_decal(culled);
    });
    CHECK(fw.last_stats().decals == 1);
    const Rgb inside = f.at_world({2.0f, 0.0f, 2.0f}), outside = f.at_world({-2.0f, 0.0f, 2.0f});
    INFO("inside " << inside0.r << "," << inside0.g << "," << inside0.b << " -> " << inside.r << "," << inside.g << "," << inside.b);
    CHECK(inside.b > inside.r + 40);
    CHECK(inside0.b < inside0.r + 10);
    CHECK(outside.r == outside0.r);
    CHECK(outside.g == outside0.g);
    CHECK(outside.b == outside0.b);
    // An emissive decal glows on the shadowed ground behind the box.
    f.render(0.0f, true, [](Scene3D& s) {
        Decal d;
        d.world = math::translation({-0.8f, 0.0f, -0.3f}) * math::scaling({0.6f, 0.6f, 0.6f});
        d.color = {1.0f, 0.2f, 0.2f, 1.0f};
        d.emissive = 2.0f;
        s.add_decal(d);
    });
    CHECK(f.at_world({-0.8f, 0.0f, -0.3f}).r > 150);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_scale: GPU particles spawn, glow over the frame and keep their ring across frames") {
    Fixture3D f;
    if (!f.device->caps().compute) return;
    const Forward3D& fw = f.renderer.forward_3d();
    f.render();
    const Array<u8> none = f.pixels;
    auto emit = [](u32 burst) {
        return [burst](Scene3D& s) {
            s.set_time(0.0f, 0.05f);
            ParticleEmitter e;
            e.id = 5;
            e.max_particles = 256;
            e.position = {1.5f, 1.0f, 1.5f};
            e.box = {0.3f, 0.3f, 0.3f};
            e.rate = 0.0f;
            e.burst = burst;
            e.speed_min = e.speed_max = 0.2f;
            e.gravity = {0.0f, 0.0f, 0.0f};
            e.lifetime_min = e.lifetime_max = 10.0f;
            e.size_start = e.size_end = 0.3f;
            s.add_particle_emitter(e);
        };
    };
    f.render(0.0f, true, emit(200));
    CHECK(fw.last_stats().particle_emitters == 1);
    CHECK(fw.last_stats().particles_spawned == 200);
    CHECK_FALSE(fw.particle_buffer(5).is_null());
    const Rgb glow = f.at_world({1.5f, 1.0f, 1.5f});
    CHECK(differing(f.pixels, none) > 0);
    CHECK(glow.r > 200);  // additive HDR sprites
    // Next frame: no new ones, the live ones still there.
    f.render(0.0f, true, emit(0));
    CHECK(fw.last_stats().particles_spawned == 0);
    CHECK(differing(f.pixels, none) > 0);
    f.render();
    CHECK(fw.particle_buffer(5).is_null());  // not added: its ring is freed
    CHECK(differing(f.pixels, none) == 0);
    CHECK(f.device->validation_error_count() == 0);
}
