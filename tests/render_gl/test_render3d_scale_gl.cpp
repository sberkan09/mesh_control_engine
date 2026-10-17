// M-VISUAL 9 (scale) against a real driver, GL 4.5 and (as test_render_sdlgpu) the SDL GPU backend:
// instance batches culled on the GPU draw the CPU path's frame, the depth pyramid drops hidden
// instances without changing the picture, batches cast shadows and sway in the wind, projected decals
// recolor what they cover, GPU particles spawn and glow, and none of it leaves a trace when off.

#include <doctest/doctest.h>

#include "fixture3d.h"
#include "mce/render/renderer.h"

using namespace mce;
using namespace mce::render;
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

TEST_CASE("render3d_scale: a smooth decal fills the normal map's bumps under it (a puddle is flat)") {
    Fixture3D f;
    // A checker of normals tilted ±37° about Z, 16 cells across the slab.
    Array<u8> px;
    for (u32 y = 0; y < 16; ++y) {
        for (u32 x = 0; x < 16; ++x) {
            const bool odd = ((x + y) & 1) != 0;
            px.push_back(odd ? 204 : 51);  // x = ±0.6
            px.push_back(128);
            px.push_back(230);  // z = 0.8
            px.push_back(255);
        }
    }
    const TextureMipUpload mip{16, 16, std::as_bytes(px.span())};
    TextureUpload up;
    up.mips = Span<const TextureMipUpload>(&mip, 1);
    const u32 bumps = f.renderer.textures().allocate_key();
    REQUIRE(f.renderer.textures().upload(bumps, 1, up).has_value());
    MaterialOverride bumpy, plain;
    for (MaterialOverride* m : {&bumpy, &plain}) {
        for (int i = 0; i < 3; ++i) m->params.base_color[i] = 0.15f;  // dark: the lit slab stays under white
        m->params.base_color[3] = 1.0f;
        m->params.roughness = 0.8f;
    }
    bumpy.normal_key = bumps;
    mesh_fixture::Builder slab_mesh;  // the ground slab again, with UVs for the normal map
    slab_mesh.add_node(-1);
    mesh_fixture::Box g;
    g.center = {0, -0.1f, 0};
    g.half = {6, 0.1f, 6};
    g.uvs = true;
    slab_mesh.add_box(g);
    REQUIRE(f.renderer.meshes().upload(20, 1, slab_mesh.build().span()).has_value());
    const auto scene = [&](const MaterialOverride* mat) {
        return [mat](Scene3D& s) {
            MeshInstance slab;  // over the fixture's ground
            slab.mesh = 20;
            slab.world = math::translation({0.0f, 0.01f, 0.0f});
            slab.material = mat;
            s.add(slab);
            Decal d;
            d.world = math::translation({2.0f, 0.0f, 2.0f}) * math::scaling({1.5f, 1.0f, 1.5f});
            d.color = {0.1f, 0.1f, 0.12f, 1.0f};
            d.roughness = 0.0f;
            d.edge_fade = 0.0f;
            s.add_decal(d);
        };
    };
    f.render(0.0f, false, scene(&bumpy));
    const Array<u8> with_bumps = f.pixels;
    Rgb in_bumpy[3];
    const vec3 inside[3] = {{2.0f, 0.01f, 2.0f}, {1.7f, 0.01f, 2.3f}, {2.3f, 0.01f, 1.7f}};
    for (int i = 0; i < 3; ++i) in_bumpy[i] = f.at_world(inside[i]);
    f.render(0.0f, false, scene(&plain));
    CHECK(differing(f.pixels, with_bumps) > 0);  // the bumps show outside the puddle
    for (int i = 0; i < 3; ++i) {
        const Rgb p = f.at_world(inside[i]);
        INFO("inside " << i << ": bumpy " << in_bumpy[i].r << "," << in_bumpy[i].g << "," << in_bumpy[i].b << " plain " << p.r << "," << p.g
                       << "," << p.b);
        CHECK(std::abs(p.r - in_bumpy[i].r) <= 2);
        CHECK(std::abs(p.g - in_bumpy[i].g) <= 2);
        CHECK(std::abs(p.b - in_bumpy[i].b) <= 2);
    }
    CHECK(f.device->validation_error_count() == 0);
}

// ---- M-VISUAL3 6: foliage variation and wind shaping --------------------------------------------------------------

namespace {

// The blue pole's pixels: the horizontal extent and count of the pixels that are clearly bluer than the scene.
struct PoleBox {
    int min_x = 1 << 20, max_x = -1;
    usize count = 0;
};
PoleBox pole_pixels(const Fixture3D& f) {
    PoleBox out;
    for (u32 y = 0; y < kH; ++y)
        for (u32 x = 0; x < kW; ++x) {
            const u8* p = &f.pixels[(usize(y) * kW + x) * 4];
            if (p[2] > p[0] + 25 && p[2] > p[1] + 25) {
                out.min_x = std::min(out.min_x, int(x));
                out.max_x = std::max(out.max_x, int(x));
                ++out.count;
            }
        }
    return out;
}

}  // namespace

TEST_CASE("render3d_scale: foliage defaults draw today's frame; variation and gusts change it") {
    Fixture3D f;
    const Array<math::mat4> grid = box_grid();
    auto frame = [&](const std::function<void(InstanceBatch&)>& tweak) {
        f.render(0.0f, true, [&](Scene3D& s) {
            s.set_time(0.37f, 0.016f);
            InstanceBatch b = grid_batch(grid.span());
            b.wind_amplitude = 0.3f;
            b.wind_frequency = 1.0f;
            if (tweak) tweak(b);
            s.add_batch(b);
        });
        return f.pixels;
    };
    const Array<u8> base = frame({});
    const Array<u8> explicit_defaults = frame([](InstanceBatch& b) {
        b.tint_variation = {1.0f, 1.0f, 1.0f, 1.0f};
        b.wind_ext = {0.0f, 0.0f, 0.0f, 0.0f};
    });
    CHECK(differing(base, explicit_defaults) == 0);
    const Array<u8> gust = frame([](InstanceBatch& b) { b.wind_ext = {0.0f, 1.0f, 0.0f, 0.0f}; });
    CHECK(differing(base, gust) > 0);
    const Array<u8> flutter = frame([](InstanceBatch& b) { b.wind_ext = {0.0f, 0.0f, 0.1f, 0.0f}; });
    CHECK(differing(base, flutter) > 0);

    // Variation: every copy mixes toward the second colour by its own hash; the same scene draws the same frame.
    auto varied = [&] { return frame([](InstanceBatch& b) { b.tint_variation = {1.0f, 0.2f, 0.1f, 1.0f}; }); };
    const Array<u8> v1 = varied(), v2 = varied();
    CHECK(differing(v1, v2) == 0);
    CHECK(differing(base, v1) > 0);
    // The tops of the copies (each box is 0.3 m tall): the plain frame shows one colour, the varied frame several.
    auto distinct_tops = [&](const Array<u8>& px) {
        f.pixels = px;
        Array<int> reds(mem::heap());
        for (const math::mat4& m : grid) {
            const Rgb c = f.at_world({m.c[3][0], 0.3f, m.c[3][2]});
            bool seen = false;
            for (int r : reds) seen = seen || std::abs(r - c.luma()) <= 3;
            if (!seen) reds.push_back(c.luma());
        }
        return reds.size();
    };
    const usize plain_colours = distinct_tops(base), varied_colours = distinct_tops(v1);
    CHECK(varied_colours >= plain_colours + 3);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_scale: wind_height caps a 5 m mesh's sway at the amplitude") {
    Fixture3D f;
    const f32 amplitude = 0.3f;
    const Array<math::mat4> pole = [] {
        Array<math::mat4> a(mem::heap());
        a.push_back(math::translation({0.0f, 0.0f, 1.5f}) * math::scaling({0.2f, 5.0f, 0.2f}));
        return a;
    }();
    auto frame = [&](f32 wind, f32 wind_height) {
        f.render(0.0f, false, [&](Scene3D& s) {
            s.set_time(0.25f, 0.016f);  // sin(2 pi f t) = 1: near the sway's peak
            InstanceBatch b = grid_batch(pole.span());
            b.tint = {0.0f, 0.0f, 4.0f, 1.0f};
            b.wind_amplitude = wind;
            b.wind_frequency = 1.0f;
            b.wind_ext = {wind_height, 0.0f, 0.0f, 0.0f};
            s.add_batch(b);
        });
        return pole_pixels(f);
    };
    const PoleBox still = frame(0.0f, 0.0f);
    REQUIRE(still.count > 40);
    const math::vec4 a = f.view.view_proj * math::vec4({0.0f, 0.0f, 1.5f}, 1.0f), c = f.view.view_proj * math::vec4({1.0f, 0.0f, 1.5f}, 1.0f);
    const f32 px_per_m = std::fabs((c.x / c.w - a.x / a.w) * 0.5f * f32(kW));
    REQUIRE(px_per_m > 5.0f);
    auto shift = [&](const PoleBox& b) { return std::max(b.max_x - still.max_x, still.min_x - b.min_x); };
    const PoleBox capped = frame(amplitude, 5.0f);
    CHECK(capped.count > 40);
    // At most the amplitude × 1.35 (the sway's peak factor), plus a pixel of rasterization slack.
    CHECK(f32(shift(capped)) <= amplitude * 1.35f * px_per_m + 1.5f);
    CHECK(shift(capped) >= 1);  // and it does sway
    // Without a wind height the same pole bends h² × amplitude: metres, not centimetres.
    const PoleBox uncapped = frame(amplitude, 0.0f);
    CHECK(f32(shift(uncapped)) > 2.0f * px_per_m);
    CHECK(f.device->validation_error_count() == 0);
}
