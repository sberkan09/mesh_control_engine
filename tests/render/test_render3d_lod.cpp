// Mesh LOD at draw time (#158, M-KIT3D), headless: MeshStore reads the cooked levels, Scene3D picks
// the coarsest level whose error stays under the pixel budget at the instance's distance, holds it
// inside the hysteresis band, draws shadows a level coarser, and counts triangles per level.

#include <doctest/doctest.h>

#include "mce/render/mesh.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::render;
using math::vec3;

namespace {

// A unit box (12 triangles) with two coarser levels: 4 triangles (error 0.01) and 2 (error 0.05).
Array<byte> box_with_lods() {
    mesh_fixture::Builder b;
    b.add_node(-1);
    mesh_fixture::Box box;
    box.half = {0.5f, 0.5f, 0.5f};
    b.add_box(box);
    b.add_lod(0, 4, 0.01f);
    b.add_lod(0, 2, 0.05f);
    return b.build();
}

struct Frame {
    MeshStore store;
    Frame() {
        store.init(nullptr);
        const Array<byte> blob = box_with_lods();
        REQUIRE(store.upload(1, 1, blob.span()).has_value());
    }
    // One frame: instances at `zs` (down -Z), returns the scene's level per instance.
    Array<u32> levels(std::initializer_list<f32> zs, const LodSettings& lod = {}, u8* states = nullptr,
                      Scene3D::Stats* stats = nullptr) {
        Scene3D scene(store, mem::heap());
        Camera3D cam;
        cam.near_z = 0.1f;
        cam.far_z = 1000.0f;
        cam.fov_y = math::radians(60.0f);
        scene.begin(cam, {1280, 720});
        scene.set_lod(lod);
        u32 i = 0;
        for (f32 z : zs) {
            MeshInstance inst;
            inst.mesh = 1;
            inst.world = math::translation({0.0f, 0.0f, -z});
            inst.lod_state = states ? &states[i] : nullptr;
            scene.add(inst);
            ++i;
        }
        scene.build();
        Array<u32> out;
        out.resize(i, ~0u);
        for (const Scene3D::Draw& d : scene.draws()) out[d.instance] = d.lod;
        if (stats) *stats = scene.stats();
        return out;
    }
};

// Pixels per metre at distance `z` for the frame above (720 px tall, 60 degree field of view).
f32 px_per_m(f32 z) { return 720.0f * 0.5f / std::tan(math::radians(30.0f)) / z; }

// The selection measures from the nearest point of the bounding sphere (the box's half diagonal).
constexpr f32 kRadius = 0.8660254f;
f32 centre_at(f32 distance) { return distance + kRadius; }

}  // namespace

TEST_CASE("render3d lod: MeshStore reads the levels; each draw carries its level's index range") {
    MeshStore store;
    store.init(nullptr);
    const Array<byte> blob = box_with_lods();
    REQUIRE(store.upload(1, 1, blob.span()).has_value());
    const GpuMesh& m = *store.get(1);
    CHECK(m.lod_levels == 3);
    CHECK(m.submeshes[0].lod_count == 3);
    CHECK(m.lod(0, 0).index_count == 36);
    CHECK(m.lod(0, 1).index_count == 12);
    CHECK(m.lod(0, 2).index_count == 6);
    CHECK(m.lod(0, 7).index_count == 6);  // past the last level: the coarsest
    CHECK(m.lod_error[1] == doctest::Approx(0.01f));
    CHECK(m.lod_error[2] == doctest::Approx(0.05f));
    CHECK(m.lod(0, 1).first_index == 36);
}

TEST_CASE("render3d lod: screen-size selection, shadows a level coarser, per-level triangle stats") {
    Frame f;
    // Level 1's error (0.01 m) reaches one pixel at ~6.2 m, level 2's (0.05 m) at ~31 m.
    const f32 d1 = 0.01f * px_per_m(1.0f), d2 = 0.05f * px_per_m(1.0f);
    Scene3D::Stats stats;
    const Array<u32> l = f.levels({2.0f, centre_at(d1 * 1.2f), centre_at(d2 * 1.2f), 200.0f}, {}, nullptr, &stats);
    CHECK(l[0] == 0);
    CHECK(l[1] == 1);
    CHECK(l[2] == 2);
    CHECK(l[3] == 2);
    CHECK(stats.lod_instances[0] == 1);
    CHECK(stats.lod_instances[1] == 1);
    CHECK(stats.lod_instances[2] == 2);
    CHECK(stats.lod_triangles[0] == 12);
    CHECK(stats.lod_triangles[1] == 4);
    CHECK(stats.lod_triangles[2] == 2 * 2);
    CHECK(stats.triangles == 12 + 4 + 4);

    // A looser budget picks coarser levels nearer; off draws the base everywhere; forcing pins one.
    LodSettings loose;
    loose.pixel_error = 8.0f;  // level 2 from ~3.9 m
    CHECK(f.levels({centre_at(d1 * 0.9f)}, loose)[0] == 2);
    LodSettings off;
    off.enabled = false;
    CHECK(f.levels({200.0f}, off)[0] == 0);
    LodSettings forced;
    forced.force_level = 9;
    CHECK(f.levels({2.0f}, forced)[0] == 2);

    // Shadows: one level coarser than the main view, clamped to the last.
    Scene3D scene(f.store, mem::heap());
    scene.begin(Camera3D{}, {1280, 720});
    MeshInstance inst;
    inst.mesh = 1;
    inst.world = math::translation({0.0f, 0.0f, -3.0f});
    scene.add(inst);
    scene.build();
    REQUIRE(scene.draws().size() == 1);
    REQUIRE(scene.shadow_draws().size() == 1);
    CHECK(scene.draws()[0].lod == 0);
    CHECK(scene.shadow_draws()[0].lod == 1);
    CHECK(scene.shadow_draws()[0].index_count == 12);
}

TEST_CASE("render3d lod: hysteresis holds the level inside the band") {
    Frame f;
    const f32 d1 = 0.01f * px_per_m(1.0f);  // level 1 allowed past here
    u8 state[1] = {0};
    // Just past the switch distance, inside the band: a fine level stays fine...
    CHECK(f.levels({centre_at(d1 * 1.1f)}, {}, state)[0] == 0);
    CHECK(state[0] == 0);
    // ...until the error is well under the budget (pixel_error * 0.75).
    CHECK(f.levels({centre_at(d1 * 1.5f)}, {}, state)[0] == 1);
    CHECK(state[0] == 1);
    // Coming back into the band keeps the coarse level; nearer than the switch point goes fine.
    CHECK(f.levels({centre_at(d1 * 1.1f)}, {}, state)[0] == 1);
    CHECK(f.levels({centre_at(d1 * 0.8f)}, {}, state)[0] == 0);
    // Without a state the level follows the screen size exactly.
    CHECK(f.levels({centre_at(d1 * 1.1f)})[0] == 1);
}

TEST_CASE("render3d lod: a far field of copies draws at least 5x fewer triangles") {
    Frame f;
    Scene3D::Stats with, without;
    LodSettings off;
    off.enabled = false;
    f.levels({60, 70, 80, 90, 100, 110, 120, 130}, {}, nullptr, &with);
    f.levels({60, 70, 80, 90, 100, 110, 120, 130}, off, nullptr, &without);
    CHECK(without.triangles == 8 * 12);
    CHECK(with.triangles * 5 <= without.triangles);
}
