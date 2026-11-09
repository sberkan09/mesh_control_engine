// M-GI2 50d against a real driver (GL 4.5 on llvmpipe and, as test_render_sdlgpu, the SDL GPU backend): the
// irradiance volume's per-probe record (mce/render/gi_probes.h). A volume that asks for visibility stores 10 texels a
// probe; its fill runs a vis pass per face (kGiVisFS, signed distances) and kGiProjectFS writes texels 4..9: the lobes'
// mean and mean square distance, the relocation offset (none yet) and the back-face share. Nothing reads the record yet
// (50e), so a visibility volume draws byte-identically to the same volume without it. Checked through the RGBM export:
// the record of probes in a closed box against a CPU projection of the box's analytic distances (and gi_record_at on
// both), the back-face share inside a closed single-sided shell, inside an open-bottomed shell standing on a floor and
// in the open, rolling updates that leave texels 4..9 bit for bit, the export / load round trip of a 10-texel volume,
// and a 4-texel bake refused (with the warning) by a 10-texel volume. 160 x 120 frames.

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string_view>

#include "../render/mesh_fixture.h"
#include "fixture3d.h"
#include "mce/log.h"
#include "mce/render/gi_probes.h"

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
// texel directions and solid angles as the shader takes them, the analytic distance to the box's inside along each.
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
                const f32 a = math::min(t, dmax) / dmax;
                for (u32 l = 0; l < 6; ++l) {
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

TEST_CASE("render3d_gl: a visibility volume draws byte-identically to the same volume without it; the vis pass runs on fills") {
    for (u32 msaa : {1u, 4u}) {
        Fixture3D f(msaa);
        upload_probe_room(f);
        bool visibility = false;
        const auto scene = [&](Scene3D& s) {
            gi_wall_scene(s, false);
            GiVolume g = room_gi();
            g.counts[0] = 3;  // 12 probes: one batch with or without the vis pass (the fill's order matters, M-VISUAL 6)
            g.counts[1] = 2;
            g.counts[2] = 2;
            g.probes_per_frame = 4;
            g.visibility = visibility;
            s.set_gi_volume(g);
        };
        const Forward3D& fw = f.renderer.forward_3d();
        Array<Array<u8>> plain;
        u32 plain_probes = 0;
        for (int i = 0; i < 4; ++i) {
            f.render(0.0f, true, scene);
            plain.push_back(f.pixels);
            CHECK(fw.last_stats().gi_vis_draws == 0);
            if (i == 0) plain_probes = fw.last_stats().gi_probes;
        }
        CHECK(plain_probes == 12);
        CHECK(f.device->texture_desc(fw.gi_volume()).size.width == 3 * kGiTexelsL1);
        visibility = true;  // a wider texture: refilled
        for (int i = 0; i < 4; ++i) {
            f.render(0.0f, true, scene);
            INFO("msaa " << msaa << " frame " << i);
            CHECK(differing(plain[usize(i)], f.pixels) == 0);
            if (i == 0) {
                CHECK(fw.last_stats().gi_filled);
                CHECK(fw.last_stats().gi_probes == plain_probes);
                CHECK(fw.last_stats().gi_vis_draws > 0);
                MESSAGE("msaa " << msaa << ": fill of 12 probes, " << fw.last_stats().gi_draws << " lit and "
                                << fw.last_stats().gi_vis_draws << " vis draws");
            } else {
                CHECK(fw.last_stats().gi_probes == 4);  // rolling: no vis pass
                CHECK(fw.last_stats().gi_vis_draws == 0);
            }
        }
        CHECK(f.device->texture_desc(fw.gi_volume()).size.width == 3 * kGiTexelsRecord);
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
