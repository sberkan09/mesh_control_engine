#pragma once
// The 3D test fixture shared by test_render3d_gl.cpp (pixel probes) and test_golden.cpp (golden images):
// a window + device + Renderer with a box on a ground slab, rendered into a 160x120 offscreen target and
// read back, plus the scene helpers of the M-VISUAL 2 cases.

#include <doctest/doctest.h>

#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
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


// M-VISUAL3 2: a 12 m tiled floor (mesh 20, just above the ground slab) whose [normal, height] array and albedo
// carry one brick pattern, with the material both ways: `on` (parallax) and `off` (the same maps, flat).
struct ParallaxFloor {
    MaterialOverride on, off;
    u32 albedo = ~0u, array = ~0u;
};

inline ParallaxFloor make_parallax_floor(Fixture3D& f, bool tilted_normals = false) {
    constexpr u32 N = 64;
    Array<u8> albedo, layers;
    for (u32 y = 0; y < N; ++y)
        for (u32 x = 0; x < N; ++x) {
            const bool groove = (x % 16) < 3 || (y % 16) < 3;
            const u8 c = groove ? 40 : 220;
            albedo.push_back(c);
            albedo.push_back(c);
            albedo.push_back(c);
            albedo.push_back(255);
        }
    for (u32 i = 0; i < N * N; ++i) {  // layer 0: a flat normal map
        layers.push_back(tilted_normals ? 238 : 128);  // 60 degrees toward +x, or flat
        layers.push_back(128);
        layers.push_back(tilted_normals ? 191 : 255);
        layers.push_back(255);
    }
    for (u32 y = 0; y < N; ++y)  // layer 1: the height map, grooves low
        for (u32 x = 0; x < N; ++x) {
            const u8 h = ((x % 16) < 3 || (y % 16) < 3) ? 0 : 255;
            layers.push_back(h);
            layers.push_back(h);
            layers.push_back(h);
            layers.push_back(255);
        }
    ParallaxFloor p;
    const TextureMipUpload amip{N, N, std::as_bytes(albedo.span())};
    TextureUpload aup;
    aup.mips = Span<const TextureMipUpload>(&amip, 1);
    p.albedo = f.renderer.textures().allocate_key();
    REQUIRE(f.renderer.textures().upload(p.albedo, 1, aup).has_value());
    const TextureMipUpload lmip{N, N, std::as_bytes(layers.span())};
    TextureUpload lup;
    lup.layers = 2;
    lup.mips = Span<const TextureMipUpload>(&lmip, 1);
    p.array = f.renderer.textures().allocate_key();
    REQUIRE(f.renderer.textures().upload(p.array, 1, lup).has_value());

    mesh_fixture::Builder floor;
    floor.add_node(-1);
    mesh_fixture::Box fb;
    fb.center = {0, -0.098f, 0};
    fb.half = {6, 0.1f, 6};
    fb.uvs = true;
    floor.add_box(fb);
    REQUIRE(f.renderer.meshes().upload(20, 1, floor.build().span()).has_value());

    p.off.albedo_key = p.albedo;
    p.off.params.base_color[0] = p.off.params.base_color[1] = p.off.params.base_color[2] = p.off.params.base_color[3] = 1.0f;
    p.off.params.roughness = 0.9f;
    p.off.params.uv_scale_half = asset::MaterialChunk::pack_uv_scale(4.0f, 4.0f);
    p.on = p.off;
    p.on.normal_key = p.array;
    p.on.parallax = true;
    p.on.parallax_normal = tilted_normals;
    p.on.ext.set_height(asset::Guid{1, 2});
    p.on.ext.height_scale = 0.08f;
    return p;
}


// ---- M-VISUAL3 4: HDR sky ------------------------------------------------------------------------

// IEEE half from a finite, non-negative float below 65520, round to nearest even.
inline u16 to_half_bits(f32 f) {
    u32 x;
    std::memcpy(&x, &f, 4);
    const i32 e = i32((x >> 23) & 0xFFu) - 127 + 15;
    u32 m = x & 0x7FFFFFu;
    if (e <= 0) {
        if (e < -10) return 0;
        m |= 0x800000u;
        const u32 shift = u32(14 - e);
        return u16((m + (1u << (shift - 1)) - 1 + ((m >> shift) & 1u)) >> shift);
    }
    const u32 h = (u32(e) << 10) | (m >> 13);
    return u16(h + ((m & 0x1FFFu) > 0x1000u || ((m & 0x1FFFu) == 0x1000u && (h & 1u)) ? 1u : 0u));
}

// The grey 64x32 equirect of the HDR sky cases: `base` everywhere and a 8x4 patch of `hot` on the horizon
// at u = 0.5 (world +X before any rotation; equirect u = atan(z, x) / 2pi + 0.5), solid angle ~0.31 sr.
inline Array<f32> hot_sky_texels(f32 base, f32 hot, u32 hot_center_x = 32) {
    Array<f32> t;
    t.resize(64 * 32, base);
    for (u32 y = 14; y < 18; ++y)
        for (u32 x = hot_center_x - 4; x < hot_center_x + 4; ++x) t[y * 64 + x] = hot;
    return t;
}

// Uploads the grey map with a full box-filtered mip chain (averaged in float): as RGBA16F when `hdr`, else as an
// RGBA8 copy clamped to 1 (what an 8-bit sky can hold). Returns the TextureStore key.
inline u32 upload_grey_sky(Fixture3D& f, const Array<f32>& grey, bool hdr) {
    Array<Array<u8>> bytes;
    Array<TextureMipUpload> mips;
    Array<f32> cur = grey;
    for (u32 w = 64, h = 32;;) {
        Array<u8>& b = bytes.emplace_back();
        for (u32 i = 0; i < w * h; ++i) {
            const f32 v = cur[i];
            if (hdr) {
                const u16 q = to_half_bits(v), one = to_half_bits(1.0f);
                for (u16 c : {q, q, q, one}) {
                    b.push_back(u8(c & 0xFF));
                    b.push_back(u8(c >> 8));
                }
            } else {
                const u8 q = u8(std::lround(math::min(v, 1.0f) * 255.0f));
                for (u8 c : {q, q, q, u8(255)}) b.push_back(c);
            }
        }
        if (w == 1 && h == 1) break;
        const u32 nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        Array<f32> next;
        next.resize(nw * nh);
        for (u32 y = 0; y < nh; ++y)
            for (u32 x = 0; x < nw; ++x) {
                const u32 x0 = math::min(x * 2, w - 1), x1 = math::min(x * 2 + 1, w - 1);
                const u32 y0 = math::min(y * 2, h - 1), y1 = math::min(y * 2 + 1, h - 1);
                next[y * nw + x] = (cur[y0 * w + x0] + cur[y0 * w + x1] + cur[y1 * w + x0] + cur[y1 * w + x1]) * 0.25f;
            }
        cur = next;
        w = nw;
        h = nh;
    }
    u32 w = 64, h = 32;
    for (const Array<u8>& b : bytes) {
        mips.push_back(TextureMipUpload{w, h, std::as_bytes(b.span())});
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    TextureUpload up;
    up.format = hdr ? rhi::TextureFormat::RGBA16F : rhi::TextureFormat::RGBA8_UNorm;
    up.mips = mips.span();
    up.debug_name = hdr ? "test.hdr_sky" : "test.ldr_sky";
    const u32 key = f.renderer.textures().allocate_key();
    REQUIRE(f.renderer.textures().upload(key, 1, up).has_value());
    return key;
}

// A white matte UV sphere as mesh `key`: `center`, `radius` baked into the submesh.
inline void upload_sphere(Fixture3D& f, u32 key, vec3 center, f32 radius, f32 metallic = 0.0f, f32 roughness = 0.8f) {
    mesh_fixture::Builder b;
    b.add_node(-1);
    asset::MeshSubmesh sm{};
    sm.first_index = 0;
    sm.base_vertex = 0;
    sm.node = 0;
    sm.center[0] = center.x;
    sm.center[1] = center.y;
    sm.center[2] = center.z;
    sm.extent[0] = sm.extent[1] = sm.extent[2] = radius;
    sm.uv_scale[0] = sm.uv_scale[1] = 1.0f;
    for (int i = 0; i < 4; ++i) sm.material.base_color[i] = 1.0f;
    sm.material.roughness = roughness;
    sm.material.metallic = metallic;
    constexpr u32 kSeg = 32, kRings = 16;
    for (u32 r = 0; r <= kRings; ++r) {
        const f32 th = math::kPi * f32(r) / f32(kRings);
        for (u32 s = 0; s <= kSeg; ++s) {
            const f32 ph = 2.0f * math::kPi * f32(s) / f32(kSeg);
            const vec3 n{std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)};
            asset::MeshVertex q{};
            q.position[0] = mesh_fixture::Builder::snorm(n.x);
            q.position[1] = mesh_fixture::Builder::snorm(n.y);
            q.position[2] = mesh_fixture::Builder::snorm(n.z);
            const f32 inv = 1.0f / (std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z));
            f32 ox = n.x * inv, oy = n.y * inv;
            if (n.z < 0) {
                const f32 tx = (1.0f - std::fabs(oy)) * (ox >= 0 ? 1.0f : -1.0f), ty = (1.0f - std::fabs(ox)) * (oy >= 0 ? 1.0f : -1.0f);
                ox = tx;
                oy = ty;
            }
            q.normal[0] = mesh_fixture::Builder::snorm(ox);
            q.normal[1] = mesh_fixture::Builder::snorm(oy);
            b.vertices.push_back(q);
        }
    }
    for (u32 r = 0; r < kRings; ++r)
        for (u32 s = 0; s < kSeg; ++s) {
            const u16 a = u16(r * (kSeg + 1) + s), c = u16(a + kSeg + 1);
            for (u16 i : {a, u16(a + 1), c, u16(a + 1), u16(c + 1), c}) b.indices.push_back(i);
        }
    sm.vertex_count = u32(b.vertices.size());
    sm.index_count = u32(b.indices.size());
    b.submeshes.push_back(sm);
    REQUIRE(f.renderer.meshes().upload(key, 1, b.build().span()).has_value());
}

}  // namespace mce::render_test
