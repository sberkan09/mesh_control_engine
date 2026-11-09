// M-GI2 50f and 50g against a real driver (GL 4.5 on llvmpipe and, as test_render_sdlgpu, the SDL GPU backend).
// 50f, probe relocation (GiVolume::relocate): a probe 0.3 m inside a solid box's face is captured 0.1 m outside it
// (stats.gi_relocated, texel 8 through the RGBM export against gi_relocate) and the face it faced is lit as by a grid
// placed off the box; a probe beyond max_offset stays where it is and is classified off; a relocating volume whose
// probes are all free draws byte-identically to the same volume without relocation, and r.gi.visibility 0 switches it
// off with the visibility. 50g, the camera-following toroidal volume (GiVolume::follow): byte-identical to a fixed volume
// over its snapped box when filled in one batch (slots, slot offset, capture, projection and lookup agree); a step past
// the dead band scrolls one slab (stats.gi_scrolled, every other slot's texels bit for bit, the slab's texel 9 at 1 until
// captured), and so do -x, -z and wrapping three-column steps (exactly gi_scroll's slots); moving within the dead band and
// a 1000 m floating-origin rebase change nothing; a camera swaying across a cell edge scrolls once, and rolling updates
// run while the queue drains; probes that scrolled in and are not captured yet weigh 0 (the pixels they alone would
// light equal the scene without a volume; in a half-scrolled cell their stale light does not leak); ten cells of movement,
// once the queue drains, are within a bound of a fresh fill there; a jump of a box refills exactly as a fresh fill; the
// Low tier and a bake keep the authored box; a later view of the frame neither scrolls nor draws with the volume; two
// runs agree; relocation runs on the scrolled-in slab too. 160 x 120 frames.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>

#include "../render/mesh_fixture.h"
#include "fixture3d.h"
#include "mce/convar.h"
#include "mce/render/gi_probes.h"
#include "mce/render/quality.h"

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

int max_diff(const Array<u8>& a, const Array<u8>& b) {
    int m = 0;
    for (usize i = 0; i < a.size(); ++i) m = math::max(m, std::abs(int(a[i]) - int(b[i])));
    return m;
}

f32 mean_diff(const Array<u8>& a, const Array<u8>& b) {
    f64 s = 0.0;
    usize n = 0;
    for (usize i = 0; i < a.size(); i += 4)
        for (usize c = 0; c < 3; ++c, ++n) s += std::abs(int(a[i + c]) - int(b[i + c]));
    return f32(s / f64(n));
}

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

// The volume as RGBM RGBA8 (Forward3D::record_gi_export), rows in the volume's v order (row = slot y + cy * slot z).
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

// Texel k of the probe in slot (sx, row) decoded from RGBM (range 16, Forward3D::kGiRgbmRange).
vec3 texel(const Array<u8>& t, rhi::Extent2D es, u32 sx, u32 row, u32 k) {
    const usize i = (usize(row) * es.width + sx * kGiTexelsRecord + k) * 4;
    const f32 m = f32(t[i + 3]) / 255.0f * 16.0f;
    return {f32(t[i]) / 255.0f * m, f32(t[i + 1]) / 255.0f * m, f32(t[i + 2]) / 255.0f * m};
}

// The bytes of the probe in slot (sx, row): its 10 texels.
bool same_probe(const Array<u8>& a, const Array<u8>& b, rhi::Extent2D es, u32 sx, u32 row) {
    const usize i = (usize(row) * es.width + sx * kGiTexelsRecord) * 4;
    return std::memcmp(&a[i], &b[i], kGiTexelsRecord * 4) == 0;
}

// Renders `scene` until the volume is filled (at most `frames` frames).
void fill(Fixture3D& f, const std::function<void(Scene3D&)>& scene, int frames = 24) {
    for (int i = 0; i < frames; ++i) {
        f.render(0.0f, true, scene);
        if (f.renderer.forward_3d().last_stats().gi_filled) return;
    }
    FAIL("the volume did not fill");
}

// The number of slots of the exported volume whose texel 9 holds kGiInvalidShare (scrolled in, not captured yet).
u32 invalid_slots(const Array<u8>& t, rhi::Extent2D es, u32 cx) {
    u32 n = 0;
    for (u32 row = 0; row < es.height; ++row)
        for (u32 sx = 0; sx < cx; ++sx) n += texel(t, es, sx, row, kGiTexelState).x > 0.99f;
    return n;
}

// Renders until the scroll queue drains (no slot of the exported volume invalid any more), at most `frames` frames;
// returns them. Read from the volume, not from stats.gi_probes, so it also ends with rolling updates on.
int drain(Fixture3D& f, const std::function<void(Scene3D&)>& scene, const GiVolume& g, int frames = 24) {
    const rhi::Extent2D es = gi_volume_size(g);
    for (int i = 0; i < frames; ++i) {
        f.render(0.0f, true, scene);
        if (invalid_slots(export_volume(f, es), es, g.counts[0]) == 0) return i + 1;
    }
    FAIL("the scroll queue did not drain");
    return frames;
}

f32 luma_at(const Fixture3D& f, Span<const vec3> points) {
    f32 s = 0.0f;
    for (const vec3& p : points) s += f32(f.at_world(p).luma());
    return s / f32(points.size());
}

// ---- 50f

// A closed single-sided solid box (mesh `key`), grey.
void upload_solid(Fixture3D& f, u32 key, vec3 center, vec3 half) {
    mesh_fixture::Builder b;
    b.add_node(-1);
    mesh_fixture::Box box;
    box.center = center;
    box.half = half;
    box.color[0] = box.color[1] = box.color[2] = 0.7f;
    b.add_box(box);
    REQUIRE(f.renderer.meshes().upload(key, 1, b.build().span()).has_value());
}

// room_gi's probe (1, 1.2, 1) (slot x 2, y 1, z 2: row 1 + 3 * 2): 0.3 m inside the +z face (z 1.3) of a 1 x 1 x 1.6 m
// box, 0.5 m from its x and y faces. gi_relocate pushes it to z 1.4: an offset of 0.4 m (max_offset 0.4 of 2 m cells
// allows 0.8). No other probe of room_gi lies in the box, the fixture's box, the ground or the red wall.
constexpr u32 kSolidMesh = 70;
const vec3 kSolidCenter{1.0f, 1.2f, 0.5f};
const vec3 kSolidHalf{0.5f, 0.5f, 0.8f};
constexpr u32 kBuriedX = 2, kBuriedRow = 1 + 3 * 2;

}  // namespace

TEST_CASE("render3d_gl: relocation moves a probe 0.3 m inside a solid box out of it; the face it faced is lit as from a grid off the box") {
    const vec3 face[3] = {{0.8f, 1.0f, 1.3f}, {1.2f, 1.2f, 1.3f}, {1.0f, 1.4f, 1.3f}};  // on the box's +z face
    f32 l[3] = {};
    // 0: the reference, the grid shifted 0.4 m on z (the probe stands where relocation puts it, no probe in a solid);
    // 1: buried (visibility only: classified off); 2: relocated.
    for (int mode = 0; mode < 3; ++mode) {
        Fixture3D f;
        upload_probe_room(f);
        upload_solid(f, kSolidMesh, kSolidCenter, kSolidHalf);
        GiVolume g = room_gi();
        g.visibility = true;
        g.relocate = mode == 2;
        if (mode == 0) {
            g.bounds.min.z += 0.4f;
            g.bounds.max.z += 0.4f;
        }
        const auto scene = [&](Scene3D& s) {
            gi_wall_scene(s, false);
            MeshInstance m;
            m.mesh = kSolidMesh;
            s.add(m);
            s.set_gi_volume(g);
        };
        fill(f, scene);
        for (int i = 0; i < 6; ++i) f.render(0.0f, true, scene);
        const Forward3D& fw = f.renderer.forward_3d();
        l[mode] = luma_at(f, Span<const vec3>(face, 3));
        CHECK(fw.last_stats().gi_relocated == (mode == 2 ? 1u : 0u));
        const rhi::Extent2D es = gi_volume_size(g);
        const Array<u8> t = export_volume(f, es);
        const vec3 cell = gi_cell(g.bounds, g.counts);
        if (mode == 2) {
            // Texel 8 decodes to gi_relocate's offset (RGBM: about 8 bits of 0.9 cell).
            const vec3 probe{1.0f, 1.2f, 1.0f};
            const GiSolid solid{math::translation(kSolidCenter), {-kSolidHalf, kSolidHalf}, kSolidCenter, math::length(kSolidHalf)};
            const vec3 expect = gi_relocate(probe, cell, g.max_offset, Span<const GiSolid>(&solid, 1));
            CHECK(expect.z == doctest::Approx(0.4f).epsilon(1e-4));
            const vec3 got = gi_offset_decode(texel(t, es, kBuriedX, kBuriedRow, kGiTexelOffset), cell);
            MESSAGE("relocated probe: offset " << got.x << ", " << got.y << ", " << got.z << " (gi_relocate " << expect.z << ")");
            CHECK(std::fabs(got.x - expect.x) <= 0.01f);
            CHECK(std::fabs(got.y - expect.y) <= 0.01f);
            CHECK(std::fabs(got.z - expect.z) <= 0.01f);
            // Captured 0.1 m in front of the face: not classified off.
            CHECK(texel(t, es, kBuriedX, kBuriedRow, kGiTexelState).x < kGiBackfaceOff);
        } else if (mode == 1) {
            CHECK(texel(t, es, kBuriedX, kBuriedRow, kGiTexelState).x >= kGiBackfaceOff);  // inside: classified off
        }
        for (u32 row = 0; row < es.height; ++row)  // every other probe: no offset
            for (u32 sx = 0; sx < 4; ++sx) {
                if (mode == 2 && sx == kBuriedX && row == kBuriedRow) continue;
                const vec3 o = texel(t, es, sx, row, kGiTexelOffset);
                CHECK(std::fabs(o.x - 0.5f) < 0.01f);
                CHECK(std::fabs(o.z - 0.5f) < 0.01f);
            }
        CHECK(f.device->validation_error_count() == 0);
    }
    MESSAGE("the box's +z face: reference " << l[0] << ", buried (classified off) " << l[1] << ", relocated " << l[2]);
    CHECK(std::fabs(l[2] - l[0]) <= 4.0f);
}

TEST_CASE("render3d_gl: a probe deeper in a solid than max_offset stays where it is and is classified off") {
    // A 2.4 x 1.8 x 2.4 m box around room_gi's probe (1, 1.2, 1): its nearest face (y) is 0.9 m away, beyond 0.4 of the
    // 1 m y cell. Every other probe is outside it.
    Fixture3D f;
    upload_probe_room(f);
    upload_solid(f, kSolidMesh, {1.0f, 1.2f, 1.0f}, {1.2f, 0.9f, 1.2f});
    GiVolume g = room_gi();
    g.visibility = true;
    g.relocate = true;
    const auto scene = [&](Scene3D& s) {
        gi_wall_scene(s, false);
        MeshInstance m;
        m.mesh = kSolidMesh;
        s.add(m);
        s.set_gi_volume(g);
    };
    fill(f, scene);
    const Forward3D& fw = f.renderer.forward_3d();
    CHECK(fw.last_stats().gi_relocated == 0);
    const rhi::Extent2D es = gi_volume_size(g);
    const Array<u8> t = export_volume(f, es);
    const vec3 o = texel(t, es, kBuriedX, kBuriedRow, kGiTexelOffset);
    CHECK(std::fabs(o.x - 0.5f) < 0.01f);
    CHECK(std::fabs(o.y - 0.5f) < 0.01f);
    CHECK(std::fabs(o.z - 0.5f) < 0.01f);
    const f32 share = texel(t, es, kBuriedX, kBuriedRow, kGiTexelState).x;
    MESSAGE("deep probe: back-face share " << share);
    CHECK(share >= kGiBackfaceOff);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: relocation with every probe free draws byte-identically; r.gi.visibility 0 switches it off") {
    // room_gi in the red-wall scene: no probe in any solid, so every offset is 0 and the relocating volume (flag 4, texel
    // 8 read) draws exactly as the visibility volume without it. With r.gi.visibility 0 a relocating volume (with and
    // without the visibility) draws as the plain volume.
    for (u32 msaa : {1u, 4u}) {
        INFO("msaa " << msaa);
        const auto run = [msaa](bool visibility, bool relocate) {
            Fixture3D f(msaa);
            upload_probe_room(f);
            Array<Array<u8>> frames;
            for (int i = 0; i < 6; ++i) {
                f.render(0.0f, true, [&](Scene3D& s) {
                    gi_wall_scene(s, false);
                    GiVolume g = room_gi();
                    g.visibility = visibility;
                    g.relocate = relocate;
                    s.set_gi_volume(g);
                });
                frames.push_back(f.pixels);
                CHECK(f.renderer.forward_3d().last_stats().gi_relocated == 0);
            }
            CHECK(f.device->validation_error_count() == 0);
            return frames;
        };
        const Array<Array<u8>> vis = run(true, false), both = run(true, true);
        for (usize i = 0; i < vis.size(); ++i) CHECK(differing(vis[i], both[i]) == 0);
        const Array<Array<u8>> plain = run(false, false);
        GiCvarScope cv("r.gi.visibility", "0");
        const Array<Array<u8>> off_both = run(true, true), off_reloc = run(false, true);
        for (usize i = 0; i < plain.size(); ++i) {
            CHECK(differing(plain[i], off_both[i]) == 0);
            CHECK(differing(plain[i], off_reloc[i]) == 0);
        }
    }
}

// ---- 50g

namespace {

// The following volume of these cases: 4 x 2 x 12 probes, 2 m apart on x and z (the authored box only gives the size
// and the y band 0.2 .. 2.2), no rolling updates (a slab, a fill or nothing moves a texel). The camera's look_at is set
// by the case; the orbit camera stands 5.73 m behind it on +z and 4 m above.
GiVolume follow_volume(u32 cx = 4, u32 cy = 2, u32 cz = 12) {
    GiVolume g;
    g.enabled = true;
    g.bounds = {{0.0f, 0.2f, 0.0f}, {2.0f * f32(cx - 1), 2.2f, 2.0f * f32(cz - 1)}};
    g.counts[0] = cx;
    g.counts[1] = cy;
    g.counts[2] = cz;
    g.probes_per_frame = 0;
    g.follow = true;
    return g;
}

// A 60 x 60 m grey floor just above the fixture's ground, so the volume's leading edges stand over something.
constexpr u32 kBigFloor = 71;
void upload_big_floor(Fixture3D& f) {
    mesh_fixture::Builder b;
    b.add_node(-1);
    mesh_fixture::Box fl;
    fl.center = {0.0f, 0.005f, 0.0f};
    fl.half = {30.0f, 0.01f, 30.0f};
    fl.color[0] = fl.color[1] = fl.color[2] = 0.6f;
    b.add_box(fl);
    REQUIRE(f.renderer.meshes().upload(kBigFloor, 1, b.build().span()).has_value());
}

// The red emissive wall (moved by `shift`, the floating origin's render-space offset), the big floor, the volume.
void follow_scene(Scene3D& s, const GiVolume& g, vec3 shift = {0, 0, 0}) {
    no_sun(s);
    s.set_sky(constant_sky(0.05f, false));
    MeshInstance wall;
    wall.mesh = 8;
    wall.world = math::translation(shift);
    s.add(wall);
    MeshInstance floor;
    floor.mesh = kBigFloor;
    floor.world = math::translation(shift);
    s.add(floor);
    s.set_gi_volume(g);
}

// The following volume's world cell of its first column for the fixture's camera (what record_gi computes).
GiFollowCell snapped(const Fixture3D& f, const GiVolume& g, const f64 (&origin)[3] = {0, 0, 0}) {
    return gi_follow_origin(f64(f.view.position.x) + origin[0], f64(f.view.position.z) + origin[2],
                            gi_follow_spacing(g.bounds.min.x, g.bounds.max.x, g.counts[0]),
                            gi_follow_spacing(g.bounds.min.z, g.bounds.max.z, g.counts[2]), g.counts[0], g.counts[2]);
}

// Where a box standing at `at` goes for the fixture's camera (record_gi's gi_follow_step: the dead band around the
// box's centre).
GiFollowCell stepped(const Fixture3D& f, const GiVolume& g, GiFollowCell at, const f64 (&origin)[3] = {0, 0, 0}) {
    return gi_follow_step(at, f64(f.view.position.x) + origin[0], f64(f.view.position.z) + origin[2],
                          gi_follow_spacing(g.bounds.min.x, g.bounds.max.x, g.counts[0]),
                          gi_follow_spacing(g.bounds.min.z, g.bounds.max.z, g.counts[2]), g.counts[0], g.counts[2]);
}

// Slot `s` lies in the run of `count` slots from `first` (wrapping modulo `n`).
bool in_run(u32 s, u32 first, u32 count, u32 n) { return (s + n - first) % n < count; }

}  // namespace

TEST_CASE("render3d_gl: a following volume filled in one batch draws byte-identically to a fixed volume over its snapped box") {
    // Filled in one frame (one batch: every capture reads the cleared volume, so the fill's order does not matter), the
    // two differ only in where each probe is stored: the following volume's slot offset is not 0 on either axis. Without
    // visibility (follow alone: flags 1 | 8, texel 9 0 everywhere) against the plain 4-texel volume; with visibility on
    // both, a smaller grid (the vis pass doubles a probe's capture cost).
    for (const bool visibility : {false, true}) {
        INFO("visibility " << visibility);
        GiVolume g = visibility ? follow_volume(2, 2, 4) : follow_volume(3, 2, 4);
        g.visibility = visibility;
        g.bounds.max.z = 12.0f;  // 4 m cells on z
        Array<u8> frames[2];
        GiFollowCell fc{};
        for (int fixed = 0; fixed < 2; ++fixed) {
            Fixture3D f;
            f.renderer.meshes().release(1);  // no probe inside the fixture's box
            upload_big_floor(f);
            f.look_at = {-1.0f, 0.5f, -4.0f};  // camera at (-1, 4.5, 1.73)
            GiVolume v = g;
            if (fixed) {
                v.follow = false;
                v.bounds = gi_follow_box(fc, g.bounds, g.counts, 0.0, 0.0);
            }
            f.render(0.0f, true, [&](Scene3D& s) { follow_scene(s, v); });
            const Forward3D& fw = f.renderer.forward_3d();
            REQUIRE(fw.last_stats().gi_filled);  // one batch
            CHECK(fw.last_stats().gi_probes == g.counts[0] * g.counts[1] * g.counts[2]);
            if (!fixed) {
                fc = snapped(f, g);
                u32 o[3];
                gi_slot_offset_of(fc, g.counts, o);
                MESSAGE("snapped first column (" << fc.x << ", " << fc.z << "), slot offset (" << o[0] << ", " << o[2] << ")");
                CHECK(o[0] != 0);
                CHECK(o[2] != 0);
            }
            for (int i = 0; i < 2; ++i) f.render(0.0f, true, [&](Scene3D& s) { follow_scene(s, v); });
            CHECK(fw.last_stats().gi);
            CHECK(fw.last_stats().gi_visibility == visibility);
            frames[fixed] = f.pixels;
            CHECK(f.device->validation_error_count() == 0);
        }
        CHECK(differing(frames[0], frames[1]) == 0);
    }
}

TEST_CASE("render3d_gl: the following volume scrolls one slab per cell crossed, keeps every other probe bit for bit; moving within a cell and a rebase change nothing") {
    Fixture3D f;
    f.renderer.meshes().release(1);
    upload_big_floor(f);
    const GiVolume g = follow_volume(4, 4, 12);  // a slab of an x step is 48 probes (more than a frame captures)
    const u32 cx = g.counts[0], cy = g.counts[1], cz = g.counts[2];
    vec3 shift{0, 0, 0};
    f64 origin[3] = {0, 0, 0};
    vec3 rebase{0, 0, 0};
    const auto scene = [&](Scene3D& s) {
        follow_scene(s, g, shift);
        if (rebase.x != 0.0f || rebase.z != 0.0f) s.set_origin_shift(rebase);
    };
    const Forward3D& fw = f.renderer.forward_3d();
    f.look_at = {1.0f, 0.5f, -4.0f};
    fill(f, scene);
    const rhi::Extent2D es = gi_volume_size(g);
    const Array<u8> before = export_volume(f, es);
    GiFollowCell at = snapped(f, g);

    // Within the dead band (the box's centre is at x 1; camera x 1 -> 1.9 -> 2.1, past the cell edge at 2 but within
    // 0.75 cell of the centre): nothing scrolls, nothing is captured, every texel stays.
    for (const f32 x : {1.9f, 2.1f}) {
        f.look_at.x = x;
        f.render(0.0f, true, scene);
        CHECK(fw.last_stats().gi_scrolled == 0);
        CHECK(fw.last_stats().gi_probes == 0);
        CHECK(stepped(f, g, at) == at);
        CHECK(differing(before, export_volume(f, es)) == 0);
    }

    // Past it (x 2.6: 0.8 cell from the centre): one slab (counts[1] * counts[2] probes) scrolls, no refill.
    f.look_at.x = 2.6f;
    f.render(0.0f, true, scene);
    const GiFollowCell to = stepped(f, g, at);
    REQUIRE(to.x == at.x + 1);
    CHECK(to.z == at.z);
    CHECK(fw.last_stats().gi_scrolled == cy * cz);
    CHECK_FALSE(fw.last_stats().gi_filled);
    CHECK(fw.last_stats().gi);
    const u32 captured = fw.last_stats().gi_probes;
    MESSAGE("scroll frame: " << fw.last_stats().gi_scrolled << " probes invalidated, " << captured << " captured");
    CHECK(captured > 0);
    const u32 slab = gi_slot(at.x, cx);  // the column that left is the one arriving
    const Array<u8> after = export_volume(f, es);
    u32 pending = 0;
    for (u32 row = 0; row < cy * cz; ++row)
        for (u32 sx = 0; sx < cx; ++sx) {
            if (sx != slab) {
                CHECK(same_probe(before, after, es, sx, row));
                continue;
            }
            const f32 state = texel(after, es, sx, row, kGiTexelState).x;
            CHECK((state > 0.9f || state == 0.0f));  // invalid (kGiInvalidShare) or captured (no back face: no vis pass)
            pending += state > 0.9f;
        }
    CHECK(pending == cy * cz - captured);
    // The queue drains; the volume keeps lighting meanwhile, every probe valid afterwards.
    const int frames = drain(f, scene, g);
    MESSAGE("queue drained in " << frames << " frames");
    CHECK(fw.last_stats().gi_scrolled == 0);
    const Array<u8> drained = export_volume(f, es);
    for (u32 row = 0; row < cy * cz; ++row)
        for (u32 sx = 0; sx < cx; ++sx) {
            CHECK(texel(drained, es, sx, row, kGiTexelState).x == 0.0f);
            if (sx != slab) CHECK(same_probe(before, drained, es, sx, row));
        }
    at = to;

    // A 1000 m rebase (x and z, not a multiple of the cell): the camera, the wall and the floor move by -d in render
    // space; the world does not move, so the volume neither scrolls nor refills, and no texel changes.
    rebase = {1000.7f, 0.0f, -999.3f};
    shift = -rebase;
    origin[0] = f64(rebase.x);
    origin[2] = f64(rebase.z);
    const Array<u8> pre = f.pixels;
    f.world_offset = shift;
    f.render(0.0f, true, scene);
    rebase = {0, 0, 0};
    CHECK(stepped(f, g, at, origin) == at);
    CHECK(fw.last_stats().gi_scrolled == 0);
    CHECK(fw.last_stats().gi_probes == 0);
    CHECK(fw.last_stats().gi);
    CHECK(differing(drained, export_volume(f, es)) == 0);
    f.render(0.0f, true, scene);
    CHECK(fw.last_stats().gi_probes == 0);
    // The frame itself: the same picture up to f32 rounding of positions 1000 m from the origin.
    const int d = max_diff(pre, f.pixels);
    MESSAGE("after the rebase: largest channel difference " << d << ", mean " << mean_diff(pre, f.pixels));
    CHECK(d <= 8);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: probes that scrolled in and are not captured yet weigh 0: the pixels they alone light are the scene's without a volume") {
    // Three cells at once (less than the box's 4): 3 * 2 * 12 = 72 probes invalidated, at most 32 captured on the
    // frame (nearest the camera first), the rest stay invalid. Every floor point whose 8 probes are all still invalid
    // must take the sky's ambient ("no probe left"), never the light the slots held before the scroll.
    const GiVolume g = follow_volume();
    const u32 cx = g.counts[0], cy = g.counts[1];
    const auto scene = [&](Scene3D& s) { follow_scene(s, g); };
    Array<u8> without;
    {
        Fixture3D ref;  // the same camera, no volume
        ref.renderer.meshes().release(1);
        upload_big_floor(ref);
        ref.look_at = {7.0f, 0.5f, -4.0f};
        GiVolume none = g;
        none.enabled = false;
        ref.render(0.0f, true, [&](Scene3D& s) { follow_scene(s, none); });
        without = ref.pixels;
    }
    Fixture3D f;
    f.renderer.meshes().release(1);
    upload_big_floor(f);
    f.look_at = {1.0f, 0.5f, -4.0f};
    fill(f, scene);
    const GiFollowCell at = snapped(f, g);
    f.look_at.x = 7.0f;  // camera cell 0 -> 3
    f.render(0.0f, true, scene);
    const Forward3D& fw = f.renderer.forward_3d();
    const GiFollowCell to = snapped(f, g);
    REQUIRE(to.x == at.x + 3);
    CHECK(fw.last_stats().gi_scrolled == 3 * cy * g.counts[2]);
    CHECK(fw.last_stats().gi);
    const rhi::Extent2D es = gi_volume_size(g);
    const Array<u8> t = export_volume(f, es);
    u32 o[3];
    gi_slot_offset_of(to, g.counts, o);
    const math::aabb3 box = gi_follow_box(to, g.bounds, g.counts, 0.0, 0.0);
    const auto invalid = [&](u32 lx, u32 ly, u32 lz) {
        return texel(t, es, gi_slot_of(lx, o[0], cx), ly + cy * gi_slot_of(lz, o[2], g.counts[2]), kGiTexelState).x > 0.9f;
    };
    u32 checked = 0, still_invalid = 0;
    for (u32 lz = 0; lz + 1 < g.counts[2]; ++lz)
        for (u32 lx = 0; lx + 1 < cx; ++lx) {
            bool all = true;
            for (u32 k = 0; k < 8; ++k) all = all && invalid(lx + (k & 1), (k >> 1) & 1, lz + (k >> 2));
            if (!all) continue;
            ++still_invalid;
            // The cell's floor centre (the lookup point, normal-biased 0.25 m up, lies inside this cell: y 0.2 .. 2.2).
            const vec3 p{box.min.x + 2.0f * (f32(lx) + 0.5f), 0.015f, box.min.z + 2.0f * (f32(lz) + 0.5f)};
            const math::vec4 c = f.view.view_proj * math::vec4(p, 1.0f);
            if (c.w <= 0.0f) continue;
            const f32 x = (c.x / c.w * 0.5f + 0.5f) * kW, y = (0.5f - c.y / c.w * 0.5f) * kH;
            if (x < 1 || y < 1 || x >= kW - 1 || y >= kH - 1) continue;
            const usize i = (usize(y) * kW + usize(x)) * 4;
            INFO("cell (" << lx << ", " << lz << ") at pixel " << x << ", " << y);
            CHECK(f.pixels[i] == without[i]);
            CHECK(f.pixels[i + 1] == without[i + 1]);
            CHECK(f.pixels[i + 2] == without[i + 2]);
            ++checked;
        }
    MESSAGE(still_invalid << " cells with every probe invalid after the scroll, " << checked << " of them on screen");
    CHECK(checked > 0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: ten cells of movement then standing still: within a bound of a fresh fill; two runs agree; a jump of a box refills as a fresh fill") {
    const GiVolume g = follow_volume();
    const auto scene = [&](Scene3D& s) { follow_scene(s, g); };
    const auto setup = [](Fixture3D& f, vec3 look_at) {
        f.renderer.meshes().release(1);
        upload_big_floor(f);
        f.look_at = look_at;
    };
    // The walk: one cell on x every frame (and one on z twice on the way), ten cells, then still until the queue drains;
    // then a jump of more than the box (5 cells on x, the box is 4). One fixture at a time (the platform is one).
    struct Walk {
        Array<Array<u8>> frames;
        u32 scrolled = 0, expected = 0;
        vec3 end{}, jump{};
        Array<u8> jumped;
    };
    const auto walk = [&](bool jump) {
        Walk w;
        Fixture3D f;
        setup(f, {1.0f, 0.5f, -4.0f});
        fill(f, scene);
        GiFollowCell at = snapped(f, g);
        for (int i = 1; i <= 10; ++i) {
            f.look_at.x = 1.0f + 2.0f * f32(i);
            if (i == 4 || i == 7) f.look_at.z -= 2.0f;
            f.render(0.0f, true, scene);
            w.scrolled += f.renderer.forward_3d().last_stats().gi_scrolled;
            // The CPU model: gi_follow_step from where the box stands, gi_scroll's probes for the step.
            const GiFollowCell to = stepped(f, g, at);
            w.expected += gi_scroll(at, to, g.counts).probes;
            at = to;
            w.frames.push_back(f.pixels);
        }
        drain(f, scene, g, 40);
        w.frames.push_back(f.pixels);
        w.end = f.look_at;
        if (jump) {
            f.look_at.x += 10.0f;
            f.render(0.0f, true, scene);
            const Forward3D& fw = f.renderer.forward_3d();
            CHECK_FALSE(fw.last_stats().gi);  // refilling: the volume stops lighting until full
            CHECK(fw.last_stats().gi_scrolled == 0);
            CHECK(fw.last_stats().gi_probes > 0);
            fill(f, scene);
            f.render(0.0f, true, scene);
            w.jump = f.look_at;
            w.jumped = f.pixels;
        }
        CHECK(f.device->validation_error_count() == 0);
        return w;
    };
    const Walk a = walk(true);
    MESSAGE("ten cells: " << a.scrolled << " probes invalidated");
    // Every frame one x slab (one cell from the centre, past the dead band); the z steps of 2 m: the first stays within
    // 0.75 cell of the centre, the second takes the box two cells at once.
    CHECK(a.scrolled == a.expected);
    CHECK(a.scrolled == 10 * g.counts[1] * g.counts[2] + 2 * g.counts[1] * (g.counts[0] - 1));
    {
        const Walk b = walk(false);
        REQUIRE(a.frames.size() == b.frames.size());
        for (usize i = 0; i < a.frames.size(); ++i) CHECK(differing(a.frames[i], b.frames[i]) == 0);  // deterministic
    }
    // A fresh fill at the end position: the probes the walk captured one slab at a time saw a volume in another state
    // (each capture reads the volume as it stands, with the probes not captured yet weighing 0), so the bounced light
    // differs a little; no rolling updates.
    {
        Fixture3D fresh;
        setup(fresh, a.end);
        fill(fresh, scene);
        fresh.render(0.0f, true, scene);
        const Array<u8>& walked = a.frames.back();
        const int d = max_diff(walked, fresh.pixels);
        const f32 m = mean_diff(walked, fresh.pixels);
        MESSAGE("after ten cells and the queue: largest channel difference to a fresh fill " << d << ", mean " << m);
        CHECK(d <= 16);
        CHECK(m <= 1.5f);
    }
    // The jump refilled exactly as a fresh fill there (the same slot offset, the same batches, a cleared volume).
    {
        Fixture3D there;
        setup(there, a.jump);
        fill(there, scene);
        there.render(0.0f, true, scene);
        CHECK(differing(a.jumped, there.pixels) == 0);
    }
}
TEST_CASE("render3d_gl: the Low tier and a bake keep the authored box; following ignores the bake where it runs") {
    // A visibility volume over the authored box, filled and exported as its bake (10 texels a probe). On the Low tier
    // (apply_quality: no updates, so no follow) a following volume loads that bake and never scrolls; a carrier that
    // keeps follow with updates off behaves the same. Where it follows (updates on), the bake is not used.
    Fixture3D f;
    upload_probe_room(f);
    GiVolume g = room_gi();
    g.visibility = true;
    const auto plain = [&](Scene3D& s) {
        gi_wall_scene(s, false);
        s.set_gi_volume(g);
    };
    fill(f, plain);
    const rhi::Extent2D es = gi_volume_size(g);
    const Array<u8> texels = export_volume(f, es);
    const TextureMipUpload mip{es.width, es.height, std::as_bytes(texels.span())};
    TextureUpload up;
    up.layers = 1;
    up.mips = Span<const TextureMipUpload>(&mip, 1);
    const u32 key = f.renderer.textures().allocate_key();
    REQUIRE(f.renderer.textures().upload(key, 1, up).has_value());
    const Forward3D& fw = f.renderer.forward_3d();
    for (int variant = 0; variant < 2; ++variant) {  // 0: apply_quality(Low), 1: follow kept with updates off
        INFO("variant " << variant);
        GiVolume low = g;
        low.follow = true;
        low.baked = key;
        if (variant == 0) {
            apply_quality(quality_settings(QualityTier::Low), low);
            CHECK_FALSE(low.follow);
            CHECK_FALSE(low.updates);
        } else {
            low.updates = false;
        }
        REQUIRE(gi_volume_size(low) == es);
        const auto scene = [&](Scene3D& s) {
            gi_wall_scene(s, false);
            s.set_gi_volume(low);
        };
        f.look_at = {0.0f, 0.5f, 0.0f};
        f.render(0.0f, true, scene);
        CHECK(fw.last_stats().gi_loaded);
        CHECK(fw.last_stats().gi);
        const Array<u8> first = f.pixels;
        for (const f32 x : {3.0f, 9.0f, 0.0f}) {  // across cells and back: nothing scrolls, nothing is captured
            f.look_at.x = x;
            f.render(0.0f, true, scene);
            CHECK(fw.last_stats().gi_scrolled == 0);
            CHECK(fw.last_stats().gi_probes == 0);
            CHECK_FALSE(fw.last_stats().gi_loaded);  // loaded once
        }
        CHECK(differing(first, f.pixels) == 0);
        f.renderer.forward_3d().invalidate_gi();
    }
    // Following with updates on: the bake (of the authored box) is not used; the volume fills at runtime.
    GiVolume med = g;
    med.follow = true;
    med.baked = key;
    f.render(0.0f, true, [&](Scene3D& s) {
        gi_wall_scene(s, false);
        s.set_gi_volume(med);
    });
    CHECK_FALSE(fw.last_stats().gi_loaded);
    CHECK(fw.last_stats().gi_probes > 0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a later view of the frame neither scrolls the following volume nor draws with it") {
    // The volume belongs to the renderer, not to a view: the frame's first view moves it; a second view (a render
    // texture's slot) with its camera 40 m away leaves it where it is and draws without it (as every later view always has).
    Fixture3D f;
    f.renderer.meshes().release(1);
    upload_big_floor(f);
    const GiVolume g = follow_volume();
    f.look_at = {1.0f, 0.5f, -4.0f};
    fill(f, [&](Scene3D& s) { follow_scene(s, g); });
    const rhi::Extent2D es = gi_volume_size(g);
    const Array<u8> before = export_volume(f, es);
    Forward3D& fw = f.renderer.forward_3d();
    rhi::TextureDesc td;
    td.size = {kW, kH};
    td.usage = rhi::TextureUsage::SampledColorTarget;
    td.debug_name = "test.second_view";
    const rhi::TextureHandle second = f.device->create_texture(td);
    for (int frame = 0; frame < 3; ++frame) {
        f.device->begin_frame(f.frame++);
        rhi::CommandBuffer cmd;
        const auto record = [&](vec3 look_at, rhi::TextureHandle target, const Forward3D::ViewOutput& out) {
            const Camera3D cam = orbit_camera(look_at, 0.0f, math::radians(35.0f), 7.0f);
            Scene3D scene(f.renderer.meshes(), mem::heap());
            scene.begin(cam, {kW, kH});
            follow_scene(scene, g);
            scene.build();
            f.renderer.record_3d(cmd, scene, target, out);
        };
        record({1.0f, 0.5f, -4.0f}, f.target, Forward3D::ViewOutput{});
        CHECK(fw.last_stats().gi);
        Forward3D::ViewOutput b;
        b.slot = 1;
        b.first = false;
        record({41.0f + 2.0f * f32(frame), 0.5f, -4.0f}, second, b);
        CHECK(fw.last_stats().gi_scrolled == 0);
        CHECK(fw.last_stats().gi_probes == 0);
        CHECK_FALSE(fw.last_stats().gi);
        f.device->submit(cmd);
        f.device->end_frame();
    }
    CHECK(differing(before, export_volume(f, es)) == 0);
    fw.release_view(1);
    f.device->destroy_texture(second);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a following volume relocates the probes of a slab that scrolled in") {
    // A solid box that only the arriving column reaches: after one cell on x, a probe of the new column (local x 3, top
    // layer, local z 6) lies 0.3 m inside its +z face (placed from the snapped box below).
    GiVolume g = follow_volume();
    g.relocate = true;
    g.visibility = true;
    Fixture3D f;
    f.renderer.meshes().release(1);
    upload_big_floor(f);
    f.look_at = {1.0f, 0.5f, -4.0f};
    // Before the scroll the box spans world x -2 .. 4 (first column -1); after one cell, 0 .. 6: the new column is x 6.
    // A 1 x 1 x 1.6 m solid with that probe 0.3 m inside its +z face, as in the 50f case.
    f.render(0.0f, true, [&](Scene3D& s) { follow_scene(s, g); });  // learn the snapped box
    const GiFollowCell at = snapped(f, g);
    const math::aabb3 now = gi_follow_box(at, g.bounds, g.counts, 0.0, 0.0);
    const vec3 probe{now.max.x + 2.0f, 2.2f, now.min.z + 2.0f * 6.0f};
    upload_solid(f, kSolidMesh, probe + vec3{0.0f, 0.0f, 0.3f - kSolidHalf.z}, kSolidHalf);
    const auto scene = [&](Scene3D& s) {
        follow_scene(s, g);
        MeshInstance m;
        m.mesh = kSolidMesh;
        s.add(m);
    };
    f.renderer.forward_3d().invalidate_gi();
    fill(f, scene);
    const Forward3D& fw = f.renderer.forward_3d();
    CHECK(fw.last_stats().gi_relocated == 0);  // no probe of the box reaches the solid yet
    f.look_at.x = 3.0f;
    f.render(0.0f, true, scene);
    CHECK(fw.last_stats().gi_scrolled == g.counts[1] * g.counts[2]);
    drain(f, scene, g);
    CHECK(fw.last_stats().gi_relocated == 1);
    const GiFollowCell to = snapped(f, g);
    u32 o[3];
    gi_slot_offset_of(to, g.counts, o);
    const rhi::Extent2D es = gi_volume_size(g);
    const Array<u8> t = export_volume(f, es);
    const vec3 cell = gi_cell(gi_follow_box(to, g.bounds, g.counts, 0.0, 0.0), g.counts);
    const vec3 off = gi_offset_decode(
        texel(t, es, gi_slot_of(g.counts[0] - 1, o[0], g.counts[0]), 1 + g.counts[1] * gi_slot_of(6, o[2], g.counts[2]), kGiTexelOffset), cell);
    MESSAGE("slab probe relocated by " << off.x << ", " << off.y << ", " << off.z);
    CHECK(std::fabs(off.z - 0.4f) <= 0.01f);
    CHECK(std::fabs(off.x) <= 0.01f);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a camera swaying across a cell edge scrolls once; rolling updates keep running while the queue drains") {
    // Rolling updates on (8 probes a frame). One step past the dead band scrolls one slab (48 probes: two frames of the
    // queue at least); the sky brightens on that frame, so the rolling update's captures show in the volume. Then the
    // camera sways 0.4 cell either side of the old cell edge (x 1.6 <-> 2.4) for twelve frames: nothing scrolls again.
    Fixture3D f;
    f.renderer.meshes().release(1);
    upload_big_floor(f);
    GiVolume g = follow_volume(4, 4, 12);
    g.probes_per_frame = 8;
    const u32 cx = g.counts[0], cy = g.counts[1], cz = g.counts[2];
    f32 sky = 0.05f;
    const auto scene = [&](Scene3D& s) {
        follow_scene(s, g);
        s.set_sky(constant_sky(sky, false));
    };
    const Forward3D& fw = f.renderer.forward_3d();
    f.look_at = {1.0f, 0.5f, -4.0f};
    fill(f, scene);
    for (int i = 0; i < 2; ++i) f.render(0.0f, true, scene);
    const rhi::Extent2D es = gi_volume_size(g);
    const Array<u8> before = export_volume(f, es);
    const GiFollowCell at = snapped(f, g);
    sky = 0.5f;
    f.look_at.x = 2.6f;
    f.render(0.0f, true, scene);
    REQUIRE(stepped(f, g, at).x == at.x + 1);
    CHECK(fw.last_stats().gi_scrolled == cy * cz);
    const u32 slab = gi_slot(at.x, cx);
    const Array<u8> after = export_volume(f, es);
    u32 pending = 0, rolled = 0;
    for (u32 row = 0; row < cy * cz; ++row)
        for (u32 sx = 0; sx < cx; ++sx) {
            if (sx == slab)
                pending += texel(after, es, sx, row, kGiTexelState).x > 0.99f;
            else
                rolled += !same_probe(before, after, es, sx, row);
        }
    MESSAGE("scroll frame with rolling updates: " << pending << " slab probes still pending, " << rolled
                                                  << " other probes re-captured by the rolling update");
    CHECK(pending > 0);  // the queue has not drained...
    CHECK(rolled > 0);   // ...and the rolling update ran anyway
    u32 scrolled = 0;
    for (int i = 0; i < 12; ++i) {
        f.look_at.x = (i & 1) ? 2.4f : 1.6f;
        f.render(0.0f, true, scene);
        scrolled += fw.last_stats().gi_scrolled;
        CHECK(fw.last_stats().gi_probes > 0);  // the queue, then the rolling update, every frame
    }
    CHECK(scrolled == 0);
    drain(f, scene, g);
    CHECK(f.device->validation_error_count() == 0);
}

namespace {

// A grey separating wall (x -1.1 .. -0.9, y 0 .. 5, z -14 .. 16) with a green emissive panel 2 m behind it (x -3.1 ..
// -2.9): the column of probes between them (x -2) holds green light, every probe on the camera's side little of it.
constexpr u32 kGreenMesh = 72;
void upload_green_room(Fixture3D& f) {
    mesh_fixture::Builder b;
    b.add_node(-1);
    mesh_fixture::Box wall;
    wall.center = {-1.0f, 2.5f, 1.0f};
    wall.half = {0.1f, 2.5f, 15.0f};
    wall.color[0] = wall.color[1] = wall.color[2] = 0.5f;
    b.add_box(wall);
    mesh_fixture::Box panel;
    panel.center = {-3.0f, 1.5f, 1.0f};
    panel.half = {0.1f, 1.5f, 15.0f};
    panel.color[0] = panel.color[1] = panel.color[2] = 0.0f;
    b.add_box(panel);
    b.submeshes.back().material.emissive[1] = 4.0f;
    REQUIRE(f.renderer.meshes().upload(kGreenMesh, 1, b.build().span()).has_value());
}

}  // namespace

TEST_CASE("render3d_gl: in a cell half scrolled in, the stale probes weigh 0: the green they held does not leak into the floor") {
    // Follow alone (flags 1 | 8, no visibility pass). Before the scroll the box's first column stands at x -2, between
    // the green panel and the wall. One cell on x moves that column's slots to x 6, on the far side; the 48 probes are
    // invalidated (their irradiance texels still hold the green, only texel 9 changes) and 32 captured, nearest first. A
    // floor point in a cell between x 4 (valid) and x 6 (still invalid), 3/4 of the way to x 6, must not take any of
    // the green: its pixel is no greener than the same pixel once the queue has drained.
    GiVolume g = follow_volume(4, 4, 12);
    const u32 cx = g.counts[0], cy = g.counts[1], cz = g.counts[2];
    Fixture3D f;
    f.renderer.meshes().release(1);
    upload_big_floor(f);
    upload_green_room(f);
    const auto scene = [&](Scene3D& s) {
        follow_scene(s, g);
        MeshInstance m;
        m.mesh = kGreenMesh;
        s.add(m);
    };
    f.look_at = {1.0f, 0.5f, -4.0f};
    fill(f, scene);
    const GiFollowCell at = snapped(f, g);
    REQUIRE(gi_follow_box(at, g.bounds, g.counts, 0.0, 0.0).min.x == doctest::Approx(-2.0f));
    f.look_at.x = 3.0f;
    f.render(0.0f, true, scene);
    const Forward3D& fw = f.renderer.forward_3d();
    const GiFollowCell to = stepped(f, g, at);
    REQUIRE(to.x == at.x + 1);
    REQUIRE(fw.last_stats().gi_scrolled == cy * cz);
    const Array<u8> scrolled = f.pixels;
    const rhi::Extent2D es = gi_volume_size(g);
    const Array<u8> t = export_volume(f, es);
    u32 o[3];
    gi_slot_offset_of(to, g.counts, o);
    const math::aabb3 box = gi_follow_box(to, g.bounds, g.counts, 0.0, 0.0);
    const auto row_of = [&](u32 ly, u32 lz) { return ly + cy * gi_slot_of(lz, o[2], cz); };
    const u32 last = gi_slot_of(cx - 1, o[0], cx), prev = gi_slot_of(cx - 2, o[0], cx);
    // The cells (local x cx - 2, lowest y layer, local z lz) whose x 6 probes are all still invalid.
    Array<u32> mixed;
    for (u32 lz = 0; lz + 1 < cz; ++lz) {
        bool stale = true;
        for (u32 k = 0; k < 4; ++k) stale = stale && texel(t, es, last, row_of(k & 1, lz + (k >> 1)), kGiTexelState).x > 0.99f;
        if (!stale) continue;
        mixed.push_back(lz);
        // The test sees the leak it guards against: the stale probes hold far more green than red, the valid ones not.
        const vec3 s = texel(t, es, last, row_of(0, lz), 0);
        const vec3 v = texel(t, es, prev, row_of(0, lz), 0);
        INFO("cell z " << lz << ": stale texel 0 " << s.x << ", " << s.y << ", " << s.z << "; valid " << v.x << ", " << v.y << ", " << v.z);
        CHECK(s.y > 4.0f * s.x);
        CHECK(s.y > 0.1f);
        CHECK(s.y > 4.0f * v.y);
    }
    REQUIRE_FALSE(mixed.empty());
    drain(f, scene, g);
    const Array<u8> drained = f.pixels;
    u32 checked = 0;
    for (const u32 lz : mixed) {
        const vec3 p{box.min.x + 2.0f * (f32(cx - 2) + 0.75f), 0.015f, box.min.z + 2.0f * (f32(lz) + 0.5f)};
        const math::vec4 c = f.view.view_proj * math::vec4(p, 1.0f);
        if (c.w <= 0.0f) continue;
        const f32 x = (c.x / c.w * 0.5f + 0.5f) * kW, y = (0.5f - c.y / c.w * 0.5f) * kH;
        if (x < 1 || y < 1 || x >= kW - 1 || y >= kH - 1) continue;
        const usize i = (usize(y) * kW + usize(x)) * 4;
        INFO("cell z " << lz << " at pixel " << x << ", " << y << ": scroll frame " << int(scrolled[i]) << ", "
                       << int(scrolled[i + 1]) << ", " << int(scrolled[i + 2]) << "; drained " << int(drained[i]) << ", "
                       << int(drained[i + 1]) << ", " << int(drained[i + 2]));
        CHECK(int(scrolled[i + 1]) <= int(drained[i + 1]) + 2);
        CHECK(int(scrolled[i + 1]) - int(scrolled[i]) <= int(drained[i + 1]) - int(drained[i]) + 2);
        ++checked;
    }
    MESSAGE(mixed.size() << " half-scrolled cells, " << checked << " of them on screen");
    CHECK(checked > 0);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("render3d_gl: a -x scroll, a -z scroll and a three-column scroll whose slot run wraps touch exactly gi_scroll's slots") {
    // 4 x 4 x 12, no rolling updates, from the box at first column -1 (slot offset 3): camera x 1 -> -0.6 steps one
    // column back (slot 2 arrives); camera z 1.73 -> -0.87 one row back; x 1 -> 7 three columns on (slots 3, 0, 1: the
    // run wraps). Every slot outside the run keeps its bytes; inside, a slot is invalid or captured this frame; once the
    // queue drains, every slot is valid and the others still hold their bytes.
    const GiVolume g = follow_volume(4, 4, 12);
    const u32 cx = g.counts[0], cy = g.counts[1], cz = g.counts[2];
    const auto scene = [&](Scene3D& s) { follow_scene(s, g); };
    const rhi::Extent2D es = gi_volume_size(g);
    struct Move {
        vec3 look_at;
        i64 dx, dz;
    };
    for (const Move mv : {Move{{-0.6f, 0.5f, -4.0f}, -1, 0}, Move{{1.0f, 0.5f, -6.6f}, 0, -1}, Move{{7.0f, 0.5f, -4.0f}, 3, 0}}) {
        INFO("look_at " << mv.look_at.x << ", " << mv.look_at.z);
        Fixture3D f;
        f.renderer.meshes().release(1);
        upload_big_floor(f);
        f.look_at = {1.0f, 0.5f, -4.0f};
        fill(f, scene);
        const GiFollowCell at = snapped(f, g);
        const Array<u8> before = export_volume(f, es);
        f.look_at = mv.look_at;
        f.render(0.0f, true, scene);
        const Forward3D& fw = f.renderer.forward_3d();
        const GiFollowCell to = stepped(f, g, at);
        REQUIRE(to.x == at.x + mv.dx);
        REQUIRE(to.z == at.z + mv.dz);
        const GiScroll sc = gi_scroll(at, to, g.counts);
        REQUIRE_FALSE(sc.refill);
        if (mv.dx == 3) CHECK(sc.x_first + sc.x_count > cx);  // the run wraps
        CHECK(fw.last_stats().gi_scrolled == sc.probes);
        CHECK_FALSE(fw.last_stats().gi_filled);
        const u32 captured = fw.last_stats().gi_probes;
        const Array<u8> after = export_volume(f, es);
        u32 in = 0, pending = 0;
        for (u32 sz = 0; sz < cz; ++sz)
            for (u32 ly = 0; ly < cy; ++ly)
                for (u32 sx = 0; sx < cx; ++sx) {
                    const u32 row = ly + cy * sz;
                    if (!in_run(sx, sc.x_first, sc.x_count, cx) && !in_run(sz, sc.z_first, sc.z_count, cz)) {
                        CHECK(same_probe(before, after, es, sx, row));
                        continue;
                    }
                    ++in;
                    const f32 state = texel(after, es, sx, row, kGiTexelState).x;
                    CHECK((state > 0.99f || state == 0.0f));
                    pending += state > 0.99f;
                }
        CHECK(in == sc.probes);
        CHECK(pending == sc.probes - captured);
        CHECK(invalid_slots(after, es, cx) == pending);
        drain(f, scene, g);
        const Array<u8> drained = export_volume(f, es);
        CHECK(invalid_slots(drained, es, cx) == 0);
        for (u32 sz = 0; sz < cz; ++sz)
            for (u32 ly = 0; ly < cy; ++ly)
                for (u32 sx = 0; sx < cx; ++sx)
                    if (!in_run(sx, sc.x_first, sc.x_count, cx) && !in_run(sz, sc.z_first, sc.z_count, cz))
                        CHECK(same_probe(before, drained, es, sx, ly + cy * sz));
        CHECK(f.device->validation_error_count() == 0);
    }
}
