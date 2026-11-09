// M-GI2 50d and 50e against a real driver (GL 4.5 on llvmpipe and, as test_render_sdlgpu, the SDL GPU backend): the
// irradiance volume's per-probe record (mce/render/gi_probes.h) and the lookup that reads it. A volume that asks for
// visibility stores 10 texels a probe; its fill runs a vis pass per face (kGiVisFS, signed distances) and kGiProjectFS
// writes texels 4..9: the lobes' mean and mean square axial distance, the relocation offset (none yet) and the back-face
// share. 50d, through the RGBM export: the record of probes in a closed box against a CPU projection of the box's
// analytic distances (and gi_record_at on both), the back-face share inside a closed single-sided shell, inside an
// open-bottomed shell standing on a floor and in the open, rolling updates that leave texels 4..9 bit for bit, the
// export / load round trip of a 10-texel volume, and a 4-texel bake refused (with the warning) by a 10-texel volume.
// 50e (gi_irradiance2's visibility weight): the shader's gi_vis against gi_visibility_weight, the thin wall, a buried
// probe, probes under a street slab, an open scene, r.gi.visibility 0, no probe left, probes on the boundary.
// 160 x 120 frames.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string_view>

#include "../render/mesh_fixture.h"
#include "fixture3d.h"
#include "mce/convar.h"
#include "mce/log.h"
#include "mce/render/gi_probes.h"
#include "shaders3d.h"  // render's 3D shaders: MCE_GLSL_GI_VIS run alone, and the fullscreen vertex stage

using namespace mce;
using namespace mce::render;
using math::vec3;
using namespace mce::render_test;

namespace {

constexpr u32 kRoomMesh = 60;   // a closed single-sided box room high above the fixture's ground
constexpr u32 kShellMesh = 61;  // an open-bottomed shell standing on the fixture's ground
const vec3 kRoomCenter{0.0f, 10.0f, 0.0f};
const vec3 kRoomHalf{2.0f, 1.5f, 2.5f};

usize differing(const Array<u8>& a, const Array<u8>& b) {
    usize n = 0;
    for (usize i = 0; i < a.size(); ++i) n += a[i] != b[i];
    return n;
}

void upload_room(Fixture3D& f) {
    mesh_fixture::Builder room;
    room.add_node(-1);
    mesh_fixture::Box b;
    b.center = kRoomCenter;
    b.half = kRoomHalf;
    room.add_box(b);
    REQUIRE(f.renderer.meshes().upload(kRoomMesh, 1, room.build().span()).has_value());
    // The shell: a 6 x 2 x 6 m box on y 0..2 without its -y face (add_box's fourth quad).
    mesh_fixture::Builder shell;
    shell.add_node(-1);
    mesh_fixture::Box s;
    s.center = {0.0f, 1.0f, 0.0f};
    s.half = {3.0f, 1.0f, 3.0f};
    shell.add_box(s);
    Array<u16> kept;
    for (u32 i = 0; i < shell.indices.size(); ++i)
        if (i / 6 != 3) kept.push_back(shell.indices[i]);
    shell.indices = kept;
    shell.submeshes.back().index_count = 30;
    REQUIRE(f.renderer.meshes().upload(kShellMesh, 1, shell.build().span()).has_value());
}

// The probes of the closed room: 2 x 2 x 2, inside it, away from its walls.
GiVolume room_volume(bool visibility) {
    GiVolume g;
    g.enabled = true;
    g.bounds = {{-1.0f, 9.5f, -1.5f}, {1.0f, 10.5f, 1.5f}};
    g.counts[0] = g.counts[1] = g.counts[2] = 2;
    g.probes_per_frame = 8;
    g.visibility = visibility;
    return g;
}

// The probes inside the open-bottomed shell, low over the floor (and clear of the fixture's box).
GiVolume shell_volume() {
    GiVolume g;
    g.enabled = true;
    g.bounds = {{-1.5f, 0.3f, -1.5f}, {1.5f, 0.9f, 1.5f}};
    g.counts[0] = g.counts[1] = g.counts[2] = 2;
    g.probes_per_frame = 8;
    g.visibility = true;
    return g;
}

// The volume as RGBM RGBA8 (Forward3D::record_gi_export), rows in the volume's v order (row = probe y + cy * z).
Array<u8> export_volume(Fixture3D& f, rhi::Extent2D es) {
    Forward3D& fw = f.renderer.forward_3d();
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
    return texels;
}

// Texel k of probe (ix, row) decoded from RGBM (range 16, Forward3D::kGiRgbmRange).
vec3 texel(const Array<u8>& t, rhi::Extent2D es, u32 ix, u32 row, u32 k) {
    const usize i = (usize(row) * es.width + ix * kGiTexelsRecord + k) * 4;
    const f32 m = f32(t[i + 3]) / 255.0f * 16.0f;
    return {f32(t[i]) / 255.0f * m, f32(t[i + 1]) / 255.0f * m, f32(t[i + 2]) / 255.0f * m};
}

GiRecord record_of(const Array<u8>& t, rhi::Extent2D es, u32 ix, u32 row) {
    GiRecord r;
    r.mean_pos = texel(t, es, ix, row, kGiTexelMeanPos);
    r.mean_neg = texel(t, es, ix, row, kGiTexelMeanNeg);
    r.square_pos = texel(t, es, ix, row, kGiTexelSquarePos);
    r.square_neg = texel(t, es, ix, row, kGiTexelSquareNeg);
    return r;
}

// kGiProjectFS's record on the CPU for a probe at `p` inside the box `lo`..`hi` (every hit a back face): the faces'
// texel directions and solid angles as the shader takes them, the analytic distance to the box's inside along each,
// taken along each lobe's axis (50e).
GiRecord reference_record(vec3 p, vec3 lo, vec3 hi, f32 dmax) {
    const vec3 fwd[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const vec3 up[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    const vec3 axes[6] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    f64 mean[6] = {}, square[6] = {}, lsum[6] = {};
    constexpr u32 kRes = Forward3D::kGiFaceSize;
    for (u32 f = 0; f < 6; ++f) {
        const vec3 right = math::cross(fwd[f], up[f]);
        for (u32 j = 0; j < kRes; ++j)
            for (u32 i = 0; i < kRes; ++i) {
                const f32 nx = (f32(i) + 0.5f) * (2.0f / f32(kRes)) - 1.0f, ny = (f32(j) + 0.5f) * (2.0f / f32(kRes)) - 1.0f;
                vec3 d = fwd[f] + right * nx + up[f] * ny;
                const f32 len2 = math::dot(d, d);
                const f32 w = 1.0f / (len2 * std::sqrt(len2));
                d = d * (1.0f / std::sqrt(len2));
                f32 t = 3.0e38f;
                const f32 dc[3] = {d.x, d.y, d.z}, pc[3] = {p.x, p.y, p.z}, lc[3] = {lo.x, lo.y, lo.z}, hc[3] = {hi.x, hi.y, hi.z};
                for (u32 a = 0; a < 3; ++a)
                    if (dc[a] != 0.0f) t = math::min(t, ((dc[a] > 0.0f ? hc[a] : lc[a]) - pc[a]) / dc[a]);
                for (u32 l = 0; l < 6; ++l) {
                    const f32 a = math::min(t * math::max(math::dot(d, axes[l]), 0.0f), dmax) / dmax;  // axial (50e)
                    const f32 lw = gi_lobe_weight(d, axes[l]) * w;
                    lsum[l] += lw;
                    mean[l] += f64(lw) * a;
                    square[l] += f64(lw) * a * a;
                }
            }
    }
    GiRecord r;
    r.mean_pos = {f32(mean[0] / lsum[0]), f32(mean[1] / lsum[1]), f32(mean[2] / lsum[2])};
    r.mean_neg = {f32(mean[3] / lsum[3]), f32(mean[4] / lsum[4]), f32(mean[5] / lsum[5])};
    r.square_pos = {f32(square[0] / lsum[0]), f32(square[1] / lsum[1]), f32(square[2] / lsum[2])};
    r.square_neg = {f32(square[3] / lsum[3]), f32(square[4] / lsum[4]), f32(square[5] / lsum[5])};
    return r;
}

f32 max_diff(vec3 a, vec3 b) { return math::max(math::max(std::fabs(a.x - b.x), std::fabs(a.y - b.y)), std::fabs(a.z - b.z)); }

vec3 probe_position(const GiVolume& g, u32 ix, u32 iy, u32 iz) {
    const vec3 t{f32(ix) / f32(g.counts[0] - 1), f32(iy) / f32(g.counts[1] - 1), f32(iz) / f32(g.counts[2] - 1)};
    return g.bounds.min + (g.bounds.max - g.bounds.min) * t;
}

// Renders `scene` until the volume is filled (at most 8 frames).
void fill(Fixture3D& f, const std::function<void(Scene3D&)>& scene) {
    for (int i = 0; i < 8; ++i) {
        f.render(0.0f, true, scene);
        if (f.renderer.forward_3d().last_stats().gi_filled) return;
    }
    FAIL("the volume did not fill in 8 frames");
}

}  // namespace

TEST_CASE("render3d_gl: the vis pass leaves a fill's light texels bit for bit; it runs on fills only") {
    // 50d drew the two volumes byte-identically (nothing read the record); from 50e on the lookup reads it, so the
    // frames differ (see the 50e cases below) and what stays is the capture: a fill's light texels (0..3), captured
    // from the cleared volume, do not depend on the vis pass that runs beside them. The scene has no ambient and no sky
    // reflection: a cleared volume's probes weigh 0 in the visibility lookup ("no probe left", the sky's ambient) where
    // the plain lookup reads their zeros, and both are black only when the sky's ambient is.
    for (u32 msaa : {1u, 4u}) {
        Fixture3D f(msaa);
        upload_probe_room(f);
        bool visibility = false;
        GiVolume g = room_gi();
        g.counts[0] = 3;  // 12 probes: one batch with or without the vis pass (the fill's order matters, M-VISUAL 6)
        g.counts[1] = 2;
        g.counts[2] = 2;
        g.probes_per_frame = 4;
        const auto scene = [&](Scene3D& s) {
            gi_wall_scene(s, false);
            DirectionalLight dark;
            dark.intensity = 0.0f;
            dark.shadows = false;
            dark.ambient_sky = dark.ambient_ground = {0, 0, 0};
            s.set_light(dark);
            Sky sky = constant_sky(0.05f, false);
            sky.specular_ambient = 0.0f;
            s.set_sky(sky);
            GiVolume v = g;
            v.visibility = visibility;
            s.set_gi_volume(v);
        };
        const Forward3D& fw = f.renderer.forward_3d();
        f.render(0.0f, true, scene);
        CHECK(fw.last_stats().gi_filled);
        CHECK(fw.last_stats().gi_probes == 12);
        CHECK(fw.last_stats().gi_vis_draws == 0);
        CHECK_FALSE(fw.last_stats().gi_visibility);
        const rhi::Extent2D e4 = gi_volume_size(g);
        CHECK(e4.width == 3 * kGiTexelsL1);
        const Array<u8> plain = export_volume(f, e4);
        visibility = true;  // a wider texture: refilled
        f.render(0.0f, true, scene);
        CHECK(fw.last_stats().gi_filled);
        CHECK(fw.last_stats().gi_probes == 12);
        CHECK(fw.last_stats().gi_vis_draws > 0);
        CHECK(fw.last_stats().gi_visibility);
        MESSAGE("msaa " << msaa << ": fill of 12 probes, " << fw.last_stats().gi_draws << " lit and "
                        << fw.last_stats().gi_vis_draws << " vis draws");
        GiVolume gv = g;
        gv.visibility = true;
        const rhi::Extent2D e10 = gi_volume_size(gv);
        CHECK(e10.width == 3 * kGiTexelsRecord);
        const Array<u8> vis = export_volume(f, e10);
        usize differ = 0;
        for (u32 row = 0; row < e4.height; ++row)
            for (u32 ix = 0; ix < 3; ++ix)
                for (u32 k = 0; k < kGiTexelsL1 * 4; ++k)
                    differ += plain[(usize(row) * e4.width + ix * kGiTexelsL1) * 4 + k] !=
                              vis[(usize(row) * e10.width + ix * kGiTexelsRecord) * 4 + k];
        CHECK(differ == 0);
        for (int i = 0; i < 3; ++i) {
            f.render(0.0f, true, scene);
            CHECK(fw.last_stats().gi_probes == 4);  // rolling: no vis pass
            CHECK(fw.last_stats().gi_vis_draws == 0);
            CHECK(fw.last_stats().gi_visibility);
        }
        CHECK(f.device->validation_error_count() == 0);
    }
}

TEST_CASE("render3d_gl: the record of probes in a closed box matches the CPU projection; a closed shell reads all back faces") {
    Fixture3D f;
    upload_room(f);
    const GiVolume g = room_volume(true);
    const auto scene = [&](Scene3D& s) {
        no_sun(s);
        MeshInstance m;
        m.mesh = kRoomMesh;
        s.add(m);
        s.set_gi_volume(g);
    };
    fill(f, scene);
    const rhi::Extent2D es = gi_volume_size(g);
    CHECK(es.width == 2 * kGiTexelsRecord);
    const Array<u8> t = export_volume(f, es);
    const vec3 lo = kRoomCenter - kRoomHalf, hi = kRoomCenter + kRoomHalf;
    const f32 dmax = gi_dmax(gi_cell(g.bounds, g.counts));
    const vec3 dirs[5] = {{1, 1, 0}, {-1, 0.5f, 2}, {0.3f, -1, -0.4f}, {-1, -1, -1}, {0, 0, 1}};
    f32 worst = 0.0f, worst_dir = 0.0f;
    for (u32 iz = 0; iz < 2; ++iz)
        for (u32 iy = 0; iy < 2; ++iy)
            for (u32 ix = 0; ix < 2; ++ix) {
                const u32 row = iy + 2 * iz;
                const GiRecord got = record_of(t, es, ix, row);
                const GiRecord want = reference_record(probe_position(g, ix, iy, iz), lo, hi, dmax);
                worst = math::max(worst, math::max(math::max(max_diff(got.mean_pos, want.mean_pos), max_diff(got.mean_neg, want.mean_neg)),
                                                   math::max(max_diff(got.square_pos, want.square_pos), max_diff(got.square_neg, want.square_neg))));
                for (const vec3& d : dirs) {
                    const GiMoments a = gi_record_at(got, d), b = gi_record_at(want, d);
                    worst_dir = math::max(worst_dir, math::max(std::fabs(a.mean - b.mean), std::fabs(a.square - b.square)));
                }
                const vec3 off = texel(t, es, ix, row, kGiTexelOffset), state = texel(t, es, ix, row, kGiTexelState);
                CHECK(max_diff(off, {0.5f, 0.5f, 0.5f}) < 0.01f);  // no relocation
                CHECK(state.x > 0.9f);                             // inside a closed single-sided shell: back faces only
                CHECK(state.y == 0.0f);
            }
    MESSAGE("closed box: worst lobe difference to the CPU projection " << worst << ", along directions " << worst_dir
                                                                       << " (of 1 = dmax " << dmax << " m)");
    CHECK(worst < 0.02f);
    CHECK(worst_dir < 0.02f);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: the back-face share of probes under an open-bottomed shell on a floor, and in the open") {
    Fixture3D f;
    upload_room(f);
    upload_probe_room(f);
    const GiVolume shell = shell_volume();
    fill(f, [&](Scene3D& s) {
        no_sun(s);
        MeshInstance m;
        m.mesh = kShellMesh;
        s.add(m);
        s.set_gi_volume(shell);
    });
    const rhi::Extent2D es = gi_volume_size(shell);
    const Array<u8> t = export_volume(f, es);
    for (u32 row = 0; row < 4; ++row)
        for (u32 ix = 0; ix < 2; ++ix) {
            const f32 share = texel(t, es, ix, row, kGiTexelState).x;
            INFO("probe " << ix << ", row " << row << ": back-face share " << share);
            MESSAGE("open-bottomed shell, probe " << ix << " row " << row << ": back-face share " << share);
            CHECK(share > 0.3f);
            CHECK(share < 0.7f);
        }
    // In the open (the GI cases' room volume: probes away from the closed wall, box and slab): no back face.
    GiVolume open = room_gi();
    open.visibility = true;
    fill(f, [&](Scene3D& s) {
        gi_wall_scene(s, false);
        s.set_gi_volume(open);
    });
    const rhi::Extent2D eo = gi_volume_size(open);
    const Array<u8> o = export_volume(f, eo);
    f32 most = 0.0f;
    for (u32 row = 0; row < eo.height; ++row)
        for (u32 ix = 0; ix < open.counts[0]; ++ix) most = math::max(most, texel(o, eo, ix, row, kGiTexelState).x);
    MESSAGE("open probes: largest back-face share " << most);
    CHECK(most < 0.01f);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: rolling updates of a visibility volume leave texels 4..9 bit for bit") {
    Fixture3D f;
    upload_probe_room(f);
    bool lamp = false;
    GiVolume g = room_gi();
    g.visibility = true;
    g.hysteresis = 0.5f;
    const auto scene = [&](Scene3D& s) {
        gi_wall_scene(s, false);
        s.set_gi_volume(g);
        if (lamp) {
            PointLight pl;
            pl.position = {-1.5f, 0.6f, 1.5f};
            pl.color = {0.2f, 1.0f, 0.2f};
            pl.intensity = 30.0f;
            pl.radius = 2.5f;
            s.add_point_light(pl);
        }
    };
    const Forward3D& fw = f.renderer.forward_3d();
    fill(f, scene);
    const rhi::Extent2D es = gi_volume_size(g);
    const Array<u8> before = export_volume(f, es);
    lamp = true;
    u32 rolled = 0;
    for (int i = 0; i < 12; ++i) {  // two sweeps of the 48 probes at 8 a frame
        f.render(0.0f, true, scene);
        CHECK_FALSE(fw.last_stats().gi_filled);
        CHECK(fw.last_stats().gi_vis_draws == 0);
        rolled += fw.last_stats().gi_probes;
    }
    CHECK(rolled == 96);
    const Array<u8> after = export_volume(f, es);
    usize record_bytes = 0, light_bytes = 0;
    for (u32 row = 0; row < es.height; ++row)
        for (u32 x = 0; x < es.width; ++x) {
            const usize i = (usize(row) * es.width + x) * 4;
            const bool rec = x % kGiTexelsRecord >= kGiTexelsL1;
            for (u32 c = 0; c < 4; ++c) (rec ? record_bytes : light_bytes) += before[i + c] != after[i + c];
        }
    MESSAGE("rolling updates with the lamp on: " << light_bytes << " light bytes changed, " << record_bytes << " record bytes");
    CHECK(light_bytes > 0);
    CHECK(record_bytes == 0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a 10-texel volume's RGBM export loads back; a 4-texel bake is refused by it with the warning") {
    Fixture3D f;
    upload_probe_room(f);
    bool updates = true, visibility = true;
    u32 baked = ~0u;
    const auto scene = [&](Scene3D& s) {
        gi_wall_scene(s, false);
        GiVolume g = room_gi();
        g.baked = baked;
        g.updates = updates;
        g.visibility = visibility;
        s.set_gi_volume(g);
    };
    Forward3D& fw = f.renderer.forward_3d();
    const vec3 spot{1.5f, 0.0f, -0.5f};
    fill(f, scene);
    const Rgb captured = f.at_world(spot);
    GiVolume g = room_gi();
    g.visibility = true;
    const rhi::Extent2D es = gi_volume_size(g);
    REQUIRE(es.width == 4 * kGiTexelsRecord);
    const Array<u8> texels = export_volume(f, es);
    const auto upload = [&](const Array<u8>& bytes, rhi::Extent2D size) {
        const TextureMipUpload mip{size.width, size.height, std::as_bytes(bytes.span())};
        TextureUpload up;
        up.layers = 1;
        up.mips = Span<const TextureMipUpload>(&mip, 1);
        const u32 key = f.renderer.textures().allocate_key();
        REQUIRE(f.renderer.textures().upload(key, 1, up).has_value());
        return key;
    };
    baked = upload(texels, es);
    updates = false;  // the Low tier: the bake only
    f.render(0.0f, true, scene);
    CHECK(fw.last_stats().gi);
    CHECK(fw.last_stats().gi_loaded);
    CHECK(fw.last_stats().gi_probes == 0);
    const Rgb loaded = f.at_world(spot);
    INFO("captured " << captured.r << "," << captured.g << " loaded " << loaded.r << "," << loaded.g);
    CHECK(std::abs(loaded.r - captured.r) <= 6);
    CHECK(std::abs(loaded.g - captured.g) <= 6);
    // The loaded record re-exports within the RGBM step.
    const Array<u8> again = export_volume(f, es);
    f32 worst = 0.0f;
    for (u32 row = 0; row < es.height; ++row)
        for (u32 ix = 0; ix < 4; ++ix)
            for (u32 k = kGiTexelsL1; k < kGiTexelsRecord; ++k)
                worst = math::max(worst, max_diff(texel(texels, es, ix, row, k), texel(again, es, ix, row, k)));
    MESSAGE("10-texel bake round trip: worst record difference " << worst);
    CHECK(worst < 0.01f);

    // A bake made before the volume asked for visibility (4 texels a probe) is refused: the runtime fill, a warning.
    visibility = false;
    baked = ~0u;
    updates = true;
    fill(f, scene);
    const rhi::Extent2D e4 = gi_volume_size(room_gi());
    const Array<u8> four = export_volume(f, e4);
    baked = upload(four, e4);
    visibility = true;
    const u64 seq = log::console_last_seq();
    f.render(0.0f, true, scene);
    log::ConsoleLine lines[32];
    const usize n = log::read_console(seq, Span<log::ConsoleLine>(lines, 32));
    bool warned = false;
    for (usize i = 0; i < n; ++i)
        warned |= std::string_view(lines[i].text.c_str()).find("GI volume bake is 16x12x1, not 40x12x1") != std::string_view::npos;
    CHECK(warned);
    CHECK_FALSE(fw.last_stats().gi_loaded);
    CHECK(fw.last_stats().gi_probes > 0);  // filled at runtime
    CHECK(fw.last_stats().gi_vis_draws > 0);
    CHECK(f.device->validation_error_count() == 0);
}

// ---- M-GI2 50e: the visibility lookup (gi_irradiance2 reads the record: Chebyshev, crush, classification) ----------

namespace {

struct GiCvarScope {  // sets a convar for the scope, resets it after
    ConVar* cv;
    GiCvarScope(const char* name, const char* value) : cv(ConVar::find(StringId(name))) {
        REQUIRE(cv != nullptr);
        REQUIRE(cv->set_string(value));
    }
    ~GiCvarScope() { cv->reset(); }
    GiCvarScope(const GiCvarScope&) = delete;
    GiCvarScope& operator=(const GiCvarScope&) = delete;
};

// Renders `scene` until the volume is filled, then `rolling` more frames.
void settle(Fixture3D& f, const std::function<void(Scene3D&)>& scene, int rolling) {
    fill(f, scene);
    for (int i = 0; i < rolling; ++i) f.render(0.0f, true, scene);
}

// The mean luma (0..255) of the pixels showing `points`.
f32 luma_at(const Fixture3D& f, Span<const vec3> points) {
    f32 s = 0.0f;
    for (const vec3& p : points) s += f32(f.at_world(p).luma());
    return s / f32(points.size());
}

// The largest per-channel difference of two frames.
int max_diff(const Array<u8>& a, const Array<u8>& b) {
    int m = 0;
    for (usize i = 0; i < a.size(); ++i) m = math::max(m, std::abs(int(a[i]) - int(b[i])));
    return m;
}

// The mean per-channel difference of two frames (rgb).
f32 mean_diff(const Array<u8>& a, const Array<u8>& b) {
    f64 s = 0.0;
    usize n = 0;
    for (usize i = 0; i < a.size(); i += 4)
        for (usize c = 0; c < 3; ++c, ++n) s += std::abs(int(a[i + c]) - int(b[i + c]));
    return f32(s / f64(n));
}

usize black_pixels(const Array<u8>& px) {
    usize n = 0;
    for (usize i = 0; i < px.size(); i += 4) n += px[i] == 0 && px[i + 1] == 0 && px[i + 2] == 0;
    return n;
}

}  // namespace

TEST_CASE("render3d_gl: the shader's visibility weight (gi_vis) is gi_visibility_weight on sampled configurations") {
    // MCE_GLSL_GI_VIS run alone: one row of kN pixels, each evaluating one configuration from a uniform block and
    // writing the f32 result's bits as four bytes (read back exactly). One row, so the backends' row order does not
    // matter. The configurations: random records, shares, directions and distances, plus the edges (the point on the
    // probe, an all-zero record, a share exactly at kGiBackfaceOff, axis directions with a -0 component).
    Fixture3D f;
    constexpr u32 kN = 64;
    constexpr const char* kFS = "#version 450 core\n" MCE_GLSL_GI_VIS R"(
layout(std140, binding = 1) uniform VisU { vec4 c[384]; };  // per configuration: v + dmax, mp, mn, sp, sn, share
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
    int i = int(gl_FragCoord.x) * 6;
    float w = gi_vis(c[i].xyz, c[i].w, c[i + 1].xyz, c[i + 2].xyz, c[i + 3].xyz, c[i + 4].xyz, c[i + 5].x);
    o_color = unpackUnorm4x8(floatBitsToUint(w));
}
)";
    struct Config {
        vec3 v;
        f32 dmax;
        GiRecord r;
        f32 share;
    };
    Array<Config> cfg;
    u32 seed = 0x2545f491u;
    const auto rnd = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return f32(seed >> 8) * (1.0f / 16777216.0f);
    };
    const auto rnd3 = [&] { return vec3{rnd(), rnd(), rnd()}; };
    for (u32 i = 0; i < kN; ++i) {
        Config c;
        c.dmax = 0.5f + 8.0f * rnd();
        c.r.mean_pos = rnd3();
        c.r.mean_neg = rnd3();
        // A square at or above mean^2 (a non-negative variance) for most, below it (clamped) for some.
        const vec3 spread = rnd3() * 0.05f;
        c.r.square_pos = c.r.mean_pos * c.r.mean_pos + (i % 5 == 0 ? spread * -1.0f : spread);
        c.r.square_neg = c.r.mean_neg * c.r.mean_neg + spread;
        c.v = (rnd3() * 2.0f - vec3{1, 1, 1}) * (c.dmax * (0.2f + 1.3f * rnd()));
        c.share = i % 7 == 0 ? 0.6f : 0.2f * rnd();
        cfg.push_back(c);
    }
    cfg[1].v = {0, 0, 0};                               // the point on the probe
    cfg[2].r = GiRecord{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};  // a probe lying on a surface
    cfg[3].share = kGiBackfaceOff;                      // exactly at the threshold: off
    cfg[4].share = 1.0f;                                // a probe not captured yet (50g)
    cfg[5].v = {-0.0f, 0.0f, -3.0f};                    // along an axis, a -0 component
    cfg[6].v = {1e-7f, 0.0f, 0.0f};                     // all but on the probe
    cfg[8].r = GiRecord{};                              // a record of nothing hit (all 1)
    Array<math::vec4> u;
    for (const Config& c : cfg) {
        u.push_back(math::vec4(c.v, c.dmax));
        u.push_back(math::vec4(c.r.mean_pos, 0.0f));
        u.push_back(math::vec4(c.r.mean_neg, 0.0f));
        u.push_back(math::vec4(c.r.square_pos, 0.0f));
        u.push_back(math::vec4(c.r.square_neg, 0.0f));
        u.push_back(math::vec4(c.share, 0.0f, 0.0f, 0.0f));
    }
    rhi::GraphicsPipelineDesc pd;
    pd.vertex_shader = {shaders3d::kFullscreenVS, "fullscreen.vert"};
    pd.fragment_shader = {kFS, "gi_vis_test.frag"};
    pd.state.depth = {};
    pd.debug_name = "test.gi_vis";
    auto pipe = f.device->create_pipeline(pd);
    REQUIRE(pipe.has_value());
    rhi::TextureDesc td;
    td.size = {kN, 1};
    td.format = rhi::TextureFormat::RGBA8_UNorm;
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.gi_vis";
    const rhi::TextureHandle rt = f.device->create_texture(td);
    f.device->begin_frame(f.frame++);
    rhi::CommandBuffer cmd;
    rhi::PassAttachment o;
    o.texture = rt;
    rhi::RenderPassDesc p;
    p.colors = Span<const rhi::PassAttachment>(&o, 1);
    p.debug_name = "test.gi_vis";
    cmd.begin_pass(p);
    cmd.bind_pipeline(pipe.value());
    cmd.set_uniforms(rhi::bank::kPass, std::as_bytes(u.span()));
    cmd.draw_non_indexed(3);
    cmd.end_pass();
    f.device->submit(cmd);
    Array<u8> px;
    px.resize(usize(kN) * 4);
    REQUIRE(f.device->read_pixels(rt, rhi::Rect2D{0, 0, kN, 1}, px.span()).has_value());
    f.device->end_frame();
    CHECK(f.device->validation_error_count() == 0);
    f32 worst = 0.0f;
    u32 occluded = 0, crushed = 0, off = 0, open = 0;
    for (u32 i = 0; i < kN; ++i) {
        const u32 bits = u32(px[i * 4]) | (u32(px[i * 4 + 1]) << 8) | (u32(px[i * 4 + 2]) << 16) | (u32(px[i * 4 + 3]) << 24);
        f32 got;
        std::memcpy(&got, &bits, sizeof(got));
        const Config& c = cfg[i];
        const f32 want = gi_visibility_weight(c.r, c.share, c.v, c.dmax);
        INFO("configuration " << i << ": gpu " << got << ", cpu " << want);
        CHECK(std::isfinite(got));
        CHECK(got >= 0.0f);
        CHECK(got <= 1.0f);
        CHECK(std::fabs(got - want) <= 1e-4f);
        worst = math::max(worst, std::fabs(got - want));
        off += want == 0.0f && c.share >= kGiBackfaceOff;
        open += want == 1.0f;
        crushed += want > 0.0f && want < kGiCrush;
        occluded += want > 0.0f && want < 1.0f;
    }
    MESSAGE("gi_vis against gi_visibility_weight over " << kN << " configurations: worst difference " << worst << " ("
            << open << " open, " << occluded << " partly occluded of which " << crushed << " crushed, " << off << " off)");
    CHECK(open > 4);  // the sample covers every branch
    CHECK(occluded > 4);
    CHECK(crushed > 2);
    CHECK(off > 4);
    f.device->destroy_texture(rt);
    f.device->destroy_pipeline(pipe.value());
}

TEST_CASE("render3d_gl: a thin wall stops leaking the sunlit room's probes into the dark room with visibility on") {
    // Measured: the dark room's floor 0.5 m from the shared wall (and deeper inside) with the volume over both rooms,
    // visibility off and on, against the reference volume confined to the dark room.
    const vec3 strip[3] = {{-1.0f, 0.0f, kThinWallZ + 0.5f}, {0.0f, 0.0f, kThinWallZ + 0.5f}, {1.0f, 0.0f, kThinWallZ + 0.5f}};
    const vec3 deep[2] = {{-1.5f, 0.0f, 1.0f}, {1.5f, 0.0f, 1.0f}};
    f32 strip_l[3] = {}, deep_l[3] = {};
    Rgb strip_c[3] = {};
    for (int mode = 0; mode < 3; ++mode) {  // 0: reference, 1: over both rooms without visibility, 2: with it
        Fixture3D f;
        upload_thin_wall_rooms(f);
        const GiVolume g = mode == 0 ? thin_wall_inside_gi() : thin_wall_gi(mode == 2);
        const auto scene = [&](Scene3D& s) {
            thin_wall_scene(s);
            s.set_gi_volume(g);
        };
        settle(f, scene, 6);
        const Forward3DStats& st = f.renderer.forward_3d().last_stats();
        CHECK(st.gi);
        CHECK(st.gi_visibility == (mode == 2));
        strip_l[mode] = luma_at(f, Span<const vec3>(strip, 3));
        deep_l[mode] = luma_at(f, Span<const vec3>(deep, 2));
        strip_c[mode] = f.at_world(strip[1]);
        CHECK(f.device->validation_error_count() == 0);
    }
    MESSAGE("thin wall, floor 0.5 m inside: reference " << strip_l[0] << ", visibility off " << strip_l[1] << ", on "
            << strip_l[2] << " (luma); deeper inside: " << deep_l[0] << ", " << deep_l[1] << ", " << deep_l[2]);
    MESSAGE("thin wall, strip centre rgb: reference " << strip_c[0].r << "," << strip_c[0].g << "," << strip_c[0].b
            << " off " << strip_c[1].r << "," << strip_c[1].g << "," << strip_c[1].b << " on " << strip_c[2].r << ","
            << strip_c[2].g << "," << strip_c[2].b);
    // The leak today: the sunlit probes brighten (and warm) the strip well past the reference.
    CHECK(strip_l[1] > strip_l[0] + 20.0f);
    CHECK(strip_c[1].r > strip_c[0].r + 25);
    // With visibility: within a few steps of the reference; and no darker than the reference by much (the dark room's
    // own probes still light it).
    CHECK(std::fabs(strip_l[2] - strip_l[0]) <= 6.0f);
    CHECK(std::abs(strip_c[2].r - strip_c[0].r) <= 8);
    CHECK(std::fabs(deep_l[2] - deep_l[0]) <= 6.0f);
}

TEST_CASE("render3d_gl: a probe buried in a solid box is classified off and stops darkening the floor beside it") {
    // A two-sided 0.9 m box (its inside drawn, so the buried probe sees dark faces) around room_gi's probe (1, 0.2, 1),
    // in the GI cases' red-wall scene. The reference: the same grid shifted 0.6 m on x, no probe buried.
    constexpr u32 kSolid = 63;
    const vec3 beside[2] = {{1.7f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.7f}};
    f32 l[3] = {};
    for (int mode = 0; mode < 3; ++mode) {  // 0: reference, 1: buried without visibility, 2: with it
        Fixture3D f;
        upload_probe_room(f);
        mesh_fixture::Builder b;
        b.add_node(-1);
        mesh_fixture::Box box;
        box.center = {1.0f, 0.45f, 1.0f};
        box.half = {0.45f, 0.45f, 0.45f};
        box.color[0] = box.color[1] = box.color[2] = 0.7f;
        box.flags = u32(asset::MaterialFlags::DoubleSided);
        b.add_box(box);
        REQUIRE(f.renderer.meshes().upload(kSolid, 1, b.build().span()).has_value());
        GiVolume g = room_gi();
        if (mode == 0) {
            g.bounds.min.x += 0.6f;
            g.bounds.max.x += 0.6f;
        }
        g.visibility = mode == 2;
        const auto scene = [&](Scene3D& s) {
            gi_wall_scene(s, false);
            MeshInstance m;
            m.mesh = kSolid;
            s.add(m);
            s.set_gi_volume(g);
        };
        settle(f, scene, 6);
        l[mode] = luma_at(f, Span<const vec3>(beside, 2));
        if (mode == 2) {
            const rhi::Extent2D es = gi_volume_size(g);
            const Array<u8> t = export_volume(f, es);
            const f32 share = texel(t, es, 2, 0 + 3 * 2, kGiTexelState).x;  // probe (1, 0.2, 1): x 2, y 0, z 2
            MESSAGE("buried probe: back-face share " << share);
            CHECK(share >= kGiBackfaceOff);
            f32 others = 0.0f;
            for (u32 row = 0; row < es.height; ++row)
                for (u32 ix = 0; ix < 4; ++ix)
                    if (!(ix == 2 && row == 6)) others = math::max(others, texel(t, es, ix, row, kGiTexelState).x);
            CHECK(others < kGiBackfaceOff);  // only the buried one is off
        }
        CHECK(f.device->validation_error_count() == 0);
    }
    MESSAGE("buried probe, floor beside the box: reference " << l[0] << ", visibility off " << l[1] << ", on " << l[2]);
    CHECK(l[1] < l[0] - 4.0f);
    CHECK(std::fabs(l[2] - l[0]) <= 3.0f);
}

TEST_CASE("render3d_gl: probes under a street slab stop darkening it with visibility on") {
    // The city's geometry at a smaller scale: a 0.5 m slab whose top is the street, the volume's bottom layer 2 m under
    // it, 16 m between probe columns and 9.33 m between layers (y -2, 7.33, 16.67, 26, as 09_openworld's volume), no sun,
    // a bright sky over a black ground: the probes over the street see the sky, the buried ones the slab's unlit
    // underside. A street point in the middle of the cell takes about half its weight from the buried layer
    // (Investigation). The reference: the same volume without its buried layer (its floor the 7.33 m layer, a fade
    // distance long enough to reach the street): what the lookup should give once the buried probes weigh nothing (the
    // normalised weights of the layer above are the same).
    constexpr u32 kSlab = 64;
    const vec3 street[3] = {{-2.0f, 0.0f, 2.0f}, {2.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 2.5f}};
    f32 l[3] = {};
    for (int mode = 0; mode < 3; ++mode) {  // 0: reference, 1: the bottom layer buried, no visibility, 2: with it
        Fixture3D f;
        f.renderer.meshes().release(1);
        f.renderer.meshes().release(2);
        mesh_fixture::Builder b;
        b.add_node(-1);
        mesh_fixture::Box slab;
        slab.center = {0.0f, -0.25f, 0.0f};
        slab.half = {20.0f, 0.25f, 20.0f};
        slab.color[0] = slab.color[1] = slab.color[2] = 0.7f;
        b.add_box(slab);
        REQUIRE(f.renderer.meshes().upload(kSlab, 1, b.build().span()).has_value());
        GiVolume g;
        g.enabled = true;
        g.bounds = {{-8.0f, -2.0f, -8.0f}, {8.0f, 26.0f, 8.0f}};
        g.counts[0] = g.counts[2] = 2;
        g.counts[1] = 4;
        if (mode == 0) {
            g.bounds.min.y = -2.0f + 28.0f / 3.0f;
            g.counts[1] = 3;
            g.fade_distance = 1.0e4f;
        }
        g.probes_per_frame = 16;
        g.visibility = mode == 2;
        const auto scene = [&](Scene3D& s) {
            no_sun(s);  // the street lit by the sky through the volume: what the probes above it see
            Sky sky = constant_sky(1.0f, false);
            sky.ground = {0, 0, 0};  // below the horizon (and under the slab) nothing lights
            s.set_sky(sky);
            MeshInstance m;
            m.mesh = kSlab;
            s.add(m);
            s.set_gi_volume(g);
        };
        settle(f, scene, 6);
        l[mode] = luma_at(f, Span<const vec3>(street, 3));
        CHECK(f.device->validation_error_count() == 0);
    }
    MESSAGE("under the street: reference " << l[0] << ", visibility off " << l[1] << ", on " << l[2]);
    CHECK(l[1] < l[0] - 4.0f);
    CHECK(std::fabs(l[2] - l[0]) <= 3.0f);
}

TEST_CASE("render3d_gl: in the open, a visibility volume draws within a small bound of the volume without it") {
    // gi_wall_scene's probes stand away from every wall: Chebyshev passes visible probes (weight 1). Hysteresis 0 and two
    // sweeps of rolling updates: during a visibility volume's fill the probes not captured yet weigh 0 and the captures
    // take the sky's ambient where the plain lookup reads zeros, a difference the default hysteresis keeps for many
    // sweeps.
    Array<u8> frames[2];
    for (int vis = 0; vis < 2; ++vis) {
        Fixture3D f;
        upload_probe_room(f);
        GiVolume g = room_gi();
        g.visibility = vis == 1;
        g.hysteresis = 0.0f;  // the rolling updates replace the fill's light (see below), two sweeps
        settle(f, [&](Scene3D& s) {
            gi_wall_scene(s, false);
            s.set_gi_volume(g);
        }, 12);
        frames[vis] = f.pixels;
        CHECK(f.renderer.forward_3d().last_stats().gi_visibility == (vis == 1));
    }
    // Not 0: a probe 0.1 m in front of the red wall (z -1) reads the wall in the lobes along x too (its hits toward -z
    // are near along x), so Chebyshev dims it a little for points beside it. Measured (llvmpipe): largest 17, mean 0.02.
    const int d = max_diff(frames[0], frames[1]);
    const f32 m = mean_diff(frames[0], frames[1]);
    MESSAGE("open scene: largest channel difference with visibility " << d << ", mean " << m);
    CHECK(d <= 20);
    CHECK(m <= 0.2f);
}

TEST_CASE("render3d_gl: r.gi.visibility 0 draws a visibility volume byte-identically to the volume without it") {
    for (u32 msaa : {1u, 4u}) {
        INFO("msaa " << msaa);
        const auto scene_of = [](const bool& visibility) {
            return [&visibility](Scene3D& s) {
                gi_wall_scene(s, false);
                GiVolume g = room_gi();
                g.visibility = visibility;
                s.set_gi_volume(g);
            };
        };
        Array<Array<u8>> plain;
        {
            Fixture3D f(msaa);
            upload_probe_room(f);
            const bool visibility = false;
            for (int i = 0; i < 6; ++i) {
                f.render(0.0f, true, scene_of(visibility));
                plain.push_back(f.pixels);
                CHECK_FALSE(f.renderer.forward_3d().last_stats().gi_visibility);
            }
        }
        {
            GiCvarScope cv("r.gi.visibility", "0");
            Fixture3D f(msaa);
            upload_probe_room(f);
            const Forward3D& fw = f.renderer.forward_3d();
            bool visibility = true;
            for (int i = 0; i < 6; ++i) {
                f.render(0.0f, true, scene_of(visibility));
                INFO("frame " << i);
                CHECK(differing(plain[usize(i)], f.pixels) == 0);
                CHECK_FALSE(fw.last_stats().gi_visibility);
                CHECK(fw.last_stats().gi_vis_draws == 0);
            }
            // The layout stays (a 10-texel bake stays valid); the record is the one of a probe that sees nothing.
            CHECK(f.device->texture_desc(fw.gi_volume()).size.width == 4 * kGiTexelsRecord);
            // Back on: the volume refills with its vis pass; the lookup uses the weight once full.
            REQUIRE(cv.cv->set_string("1"));
            f.render(0.0f, true, scene_of(visibility));
            CHECK(fw.last_stats().gi_vis_draws > 0);
            fill(f, scene_of(visibility));
            CHECK(f.device->texture_desc(fw.gi_volume()).size.width == 4 * kGiTexelsRecord);
            CHECK(fw.last_stats().gi_visibility);
            CHECK(f.device->validation_error_count() == 0);
        }
    }
    // Two runs of a visibility volume agree byte for byte.
    Array<Array<u8>> runs[2];
    for (int run = 0; run < 2; ++run) {
        Fixture3D f;
        upload_thin_wall_rooms(f);
        for (int i = 0; i < 5; ++i) {
            f.render(0.0f, true, [](Scene3D& s) {
                thin_wall_scene(s);
                s.set_gi_volume(thin_wall_gi(true));
            });
            runs[run].push_back(f.pixels);
        }
    }
    for (usize i = 0; i < runs[0].size(); ++i) CHECK(differing(runs[0][i], runs[1][i]) == 0);
}

TEST_CASE("render3d_gl: no probe left: a volume whose every probe is classified off draws as the scene without it") {
    // Every probe inside a closed single-sided 3 m shell (back faces all round: classified off). With visibility the
    // lookup returns a = 0 everywhere, so the sky's ambient lights the shell and the ground as without the volume; the
    // fade reaches 3 m past the probes, so without visibility the probes (which see through the shell) light them.
    constexpr u32 kShell = 65;
    Array<u8> frames[3];
    for (int mode = 0; mode < 3; ++mode) {  // 0: no volume, 1: the volume without visibility, 2: with it
        Fixture3D f;
        f.renderer.meshes().release(1);
        mesh_fixture::Builder b;
        b.add_node(-1);
        mesh_fixture::Box shell;
        shell.center = {0.0f, 1.5f, 0.0f};
        shell.half = {1.5f, 1.5f, 1.5f};
        shell.color[0] = shell.color[1] = shell.color[2] = 0.7f;
        b.add_box(shell);
        REQUIRE(f.renderer.meshes().upload(kShell, 1, b.build().span()).has_value());
        GiVolume g;
        g.enabled = mode > 0;
        g.bounds = {{-1.0f, 0.5f, -1.0f}, {1.0f, 2.5f, 1.0f}};
        g.counts[0] = g.counts[1] = g.counts[2] = 2;
        g.fade_distance = 3.0f;
        g.visibility = mode == 2;
        const auto scene = [&](Scene3D& s) {
            s.set_sky(constant_sky(0.3f, false));
            MeshInstance m;
            m.mesh = kShell;
            s.add(m);
            s.set_gi_volume(g);
        };
        if (mode == 0)
            for (int i = 0; i < 4; ++i) f.render(0.0f, true, scene);
        else
            settle(f, scene, 3);
        frames[mode] = f.pixels;
        if (mode == 2) {
            CHECK(f.renderer.forward_3d().last_stats().gi_visibility);
            const rhi::Extent2D es = gi_volume_size(g);
            const Array<u8> t = export_volume(f, es);
            f32 least = 1.0f;
            for (u32 row = 0; row < es.height; ++row)
                for (u32 ix = 0; ix < 2; ++ix) least = math::min(least, texel(t, es, ix, row, kGiTexelState).x);
            MESSAGE("no probe left: smallest back-face share " << least);
            CHECK(least > 0.9f);
        }
        CHECK(f.device->validation_error_count() == 0);
    }
    MESSAGE("no probe left: largest difference to the scene without a volume: visibility off "
            << max_diff(frames[0], frames[1]) << ", on " << max_diff(frames[0], frames[2]));
    CHECK(max_diff(frames[0], frames[1]) > 8);
    CHECK(differing(frames[0], frames[2]) == 0);
}

TEST_CASE("render3d_gl: probes on the volume's boundary: the lookup point on a probe (r = 0), a probe in a wall's face") {
    // (a) room_gi raised so its bottom plane (y 0.3) is above the normal bias: every floor point clamps onto it, and
    // the floor right under a probe looks it up at the probe itself (r = 0: weight 1, nothing divided by zero).
    // (b) its -z plane moved into the red wall's face (z -1.1): those probes lie on a surface (a zero distance, half
    // their sphere behind the face). Either way: finite, no pixel turned black, within a bound of the volume without
    // visibility where the probes are in the open.
    for (int variant = 0; variant < 2; ++variant) {
        Array<u8> frames[2];
        Rgb under[2] = {};
        for (int vis = 0; vis < 2; ++vis) {
            Fixture3D f;
            upload_probe_room(f);
            GiVolume g = room_gi();
            if (variant == 0) {
                g.bounds.min.y = 0.3f;
                g.bounds.max.y = 2.3f;
            } else {
                g.bounds.min.z = -1.1f;
            }
            g.visibility = vis == 1;
            g.hysteresis = 0.0f;  // as the open-scene case
            settle(f, [&](Scene3D& s) {
                gi_wall_scene(s, false);
                s.set_gi_volume(g);
            }, 12);
            frames[vis] = f.pixels;
            under[vis] = f.at_world({1.0f, 0.0f, 1.0f});  // under the probe column (1, *, 1) in variant 0
            CHECK(f.device->validation_error_count() == 0);
        }
        const int d = max_diff(frames[0], frames[1]);
        MESSAGE("boundary variant " << variant << ": largest difference " << d << ", mean " << mean_diff(frames[0], frames[1])
                                    << "; black pixels " << black_pixels(frames[0])
                                    << " -> " << black_pixels(frames[1]) << "; under the probe " << under[0].luma()
                                    << " -> " << under[1].luma());
        CHECK(black_pixels(frames[1]) == black_pixels(frames[0]));
        if (variant == 0) {
            // Not 0 for the reason of the open-scene case (the probes 0.1 m in front of the red wall); measured 14, mean
            // 0.15. Under the probe column itself (r = 0) the pixel is within 2.
            CHECK(d <= 16);
            CHECK(mean_diff(frames[0], frames[1]) <= 1.5f);
            CHECK(std::abs(under[1].luma() - under[0].luma()) <= 2);
        } else {
            CHECK(under[1].luma() > 0);
        }
    }
}

TEST_CASE("render3d_gl: r.gi.visibility 0 keeps a 10-texel bake on the Low tier and reads it with the plain lookup") {
    Fixture3D f;
    upload_probe_room(f);
    bool updates = true;
    u32 baked = ~0u;
    const auto scene = [&](Scene3D& s) {
        gi_wall_scene(s, false);
        GiVolume g = room_gi();
        g.visibility = true;
        g.baked = baked;
        g.updates = updates;
        s.set_gi_volume(g);
    };
    const Forward3D& fw = f.renderer.forward_3d();
    const vec3 spot{1.5f, 0.0f, -0.5f};
    fill(f, scene);
    const Rgb captured = f.at_world(spot);
    GiVolume g = room_gi();
    g.visibility = true;
    const rhi::Extent2D es = gi_volume_size(g);
    const Array<u8> texels = export_volume(f, es);
    const TextureMipUpload mip{es.width, es.height, std::as_bytes(texels.span())};
    TextureUpload up;
    up.layers = 1;
    up.mips = Span<const TextureMipUpload>(&mip, 1);
    baked = f.renderer.textures().allocate_key();
    REQUIRE(f.renderer.textures().upload(baked, 1, up).has_value());
    updates = false;  // the Low tier: the bake only
    Array<u8> frames[2];
    for (int on = 0; on < 2; ++on) {
        GiCvarScope cv("r.gi.visibility", on == 1 ? "1" : "0");
        f.render(0.0f, true, scene);
        INFO("r.gi.visibility " << on);
        CHECK(fw.last_stats().gi);
        CHECK(fw.last_stats().gi_loaded);
        CHECK(fw.last_stats().gi_probes == 0);
        CHECK(fw.last_stats().gi_visibility == (on == 1));
        CHECK(f.device->texture_desc(fw.gi_volume()).size.width == es.width);
        const Rgb loaded = f.at_world(spot);
        CHECK(std::abs(loaded.r - captured.r) <= 8);
        CHECK(std::abs(loaded.g - captured.g) <= 8);
        frames[on] = f.pixels;
    }
    MESSAGE("10-texel bake on Low: r.gi.visibility 0 against 1, largest difference " << max_diff(frames[0], frames[1]));
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: no probe left: a room narrower than the probe spacing, both probe planes outside its walls") {
    // The dark room of the thin-wall scene inside one cell of 2 x 2 x 2 probes: x -1 and 1, y 0.6 and 2.6 inside its
    // span, z -2.6 (in the sunlit room, 0.4 m in front of the shared wall) and 6.4 (0.4 m behind the room's back wall,
    // over a ground slab added there), 9 m apart. The probes stand well inside the walls' span: one near a wall's end
    // sees past it in the tail of its lobe, and the spread that puts in its record keeps it partly visible (at x -2,
    // 1.2 m from the end, the factor read 0.02; a limit in the as-built entry). Every point of the room's floor is behind a wall from all 8 probes, so with visibility the lookup gives
    // a = 0 and the pixels are those of the scene without a volume (the sky's ambient: none here); without visibility the
    // sunlit probes light it. (Probe planes past the box, as in the fade band, would not do: the clamped lookup point
    // then sits on their side of the wall; a limit in the as-built entry.)
    const vec3 strip[3] = {{-1.0f, 0.0f, kThinWallZ + 0.5f}, {0.0f, 0.0f, kThinWallZ + 0.5f}, {1.0f, 0.0f, kThinWallZ + 0.5f}};
    Rgb px[3][3] = {};
    for (int mode = 0; mode < 3; ++mode) {  // 0: no volume, 1: the volume without visibility, 2: with it
        Fixture3D f;
        upload_thin_wall_rooms(f);
        mesh_fixture::Builder b;  // the ground behind the room (the fixture's ends at z 6)
        b.add_node(-1);
        mesh_fixture::Box ground;
        ground.center = {0.0f, -0.1f, 9.0f};
        ground.half = {6.0f, 0.1f, 3.0f};
        ground.color[0] = ground.color[1] = ground.color[2] = 0.7f;
        b.add_box(ground);
        REQUIRE(f.renderer.meshes().upload(66, 1, b.build().span()).has_value());
        GiVolume g = thin_wall_gi(mode == 2);
        g.enabled = mode > 0;
        g.bounds = {{-1.0f, 0.6f, -2.6f}, {1.0f, 2.6f, 6.4f}};
        g.counts[0] = g.counts[1] = g.counts[2] = 2;
        const auto scene = [&](Scene3D& s) {
            thin_wall_scene(s);
            MeshInstance m;
            m.mesh = 66;
            s.add(m);
            s.set_gi_volume(g);
        };
        if (mode == 0)
            for (int i = 0; i < 4; ++i) f.render(0.0f, true, scene);
        else
            settle(f, scene, 3);
        for (int i = 0; i < 3; ++i) px[mode][i] = f.at_world(strip[i]);
        CHECK(f.device->validation_error_count() == 0);
    }
    for (int i = 0; i < 3; ++i) {
        INFO("strip point " << i << ": none " << px[0][i].luma() << ", off " << px[1][i].luma() << ", on " << px[2][i].luma());
        CHECK(px[1][i].luma() > px[0][i].luma() + 10);
        CHECK(px[2][i].r == px[0][i].r);
        CHECK(px[2][i].g == px[0][i].g);
        CHECK(px[2][i].b == px[0][i].b);
    }
    MESSAGE("room inside one cell, strip centre luma: no volume " << px[0][1].luma() << ", off " << px[1][1].luma()
            << ", on " << px[2][1].luma());
}
