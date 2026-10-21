// Scale (M-VISUAL 9), headless: the foliage scatter is deterministic and stays in its rectangle on a
// jittered grid; the CPU batch culling keeps what the frustum and cull distance keep and picks LOD
// levels as the per-instance path does; Scene3D takes batches, decals and emitters and counts them.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/render/mesh.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::render;
using math::vec3;

namespace {

bool same_matrix(const math::mat4& a, const math::mat4& b) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (a.c[c][r] != b.c[c][r]) return false;
    return true;
}

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

Camera3D camera() {
    Camera3D cam;
    cam.near_z = 0.1f;
    cam.far_z = 1000.0f;
    cam.fov_y = math::radians(60.0f);
    return cam;  // at the origin, looking down -Z
}

}  // namespace

TEST_CASE("render3d scale: scatter_foliage is deterministic, inside its rectangle and spread out") {
    FoliageScatter sc;
    sc.origin = {10.0f, 2.0f, -4.0f};
    sc.half_extents = {6.0f, 3.0f};
    sc.count = 200;
    sc.seed = 42;
    Array<math::mat4> a, b, c;
    scatter_foliage(sc, a);
    scatter_foliage(sc, b);
    sc.seed = 43;
    scatter_foliage(sc, c);
    REQUIRE(a.size() == 200);
    REQUIRE(c.size() == 200);
    usize same = 0;
    for (usize i = 0; i < a.size(); ++i) {
        CHECK(same_matrix(a[i], b[i]));
        same += same_matrix(a[i], c[i]);
        const vec3 p{a[i].c[3][0], a[i].c[3][1], a[i].c[3][2]};
        CHECK(p.x >= 4.0f);
        CHECK(p.x <= 16.0f);
        CHECK(p.z >= -7.0f);
        CHECK(p.z <= -1.0f);
        CHECK(p.y == doctest::Approx(2.0f));
        const f32 s = math::length(vec3{a[i].c[0][0], a[i].c[0][1], a[i].c[0][2]});
        CHECK(s >= 0.8f - 1e-4f);
        CHECK(s <= 1.2f + 1e-4f);
        CHECK(a[i].c[1][1] == doctest::Approx(s));  // upright: yaw only
    }
    CHECK(same == 0);
    // A jittered grid: no two instances closer than a small fraction of the cell.
    const f32 cell = std::sqrt(12.0f * 6.0f / 200.0f);
    f32 closest = 1e9f;
    for (usize i = 0; i < a.size(); ++i)
        for (usize j = i + 1; j < a.size(); ++j) {
            const f32 dx = a[i].c[3][0] - a[j].c[3][0], dz = a[i].c[3][2] - a[j].c[3][2];
            closest = math::min(closest, std::sqrt(dx * dx + dz * dz));
        }
    CHECK(closest > 0.05f * cell);
    sc.count = 0;
    scatter_foliage(sc, a);
    CHECK(a.empty());
}

TEST_CASE("render3d scale: cull_batch keeps the frustum and cull distance, and picks the LOD level") {
    MeshStore store;
    store.init(nullptr);
    const Array<byte> blob = box_with_lods();
    REQUIRE(store.upload(1, 1, blob.span()).has_value());
    const GpuMesh& mesh = *store.get(1);
    Scene3D scene(store, mem::heap());
    scene.begin(camera(), {1280, 720});
    const View3D& view = scene.view();
    math::vec4 planes[6];
    frustum_planes(view.view_proj, planes);
    const math::mat4 transforms[] = {
        math::translation({0.0f, 0.0f, -2.0f}),     // near: level 0
        math::translation({0.0f, 0.0f, -400.0f}),   // far: the coarsest level
        math::translation({0.0f, 0.0f, 5.0f}),      // behind the camera
        math::translation({300.0f, 0.0f, -10.0f}),  // off to the side
        math::translation({0.0f, 0.0f, -60.0f}),
    };
    InstanceBatch b;
    b.mesh = 1;
    b.transforms = Span<const math::mat4>(transforms, 5);
    Array<BatchVisible> out;
    LodSettings lod;
    cull_batch(b, mesh, planes, view, lod, 0, out);
    REQUIRE(out.size() == 3);
    CHECK(out[0].instance == 0);
    CHECK(out[0].level == 0);
    CHECK(out[1].instance == 1);
    CHECK(out[1].level == 2);
    CHECK(out[2].instance == 4);
    // The level matches the scene's own pick for a lone instance at the same spot (no hysteresis).
    for (const BatchVisible& v : out) {
        const vec3 c{transforms[v.instance].c[3][0], transforms[v.instance].c[3][1], transforms[v.instance].c[3][2]};
        CHECK(v.level == batch_lod(mesh, c, math::length(mesh.bounds.extents()), 1.0f, view, lod));
    }
    // Shadows draw a level coarser; a cull distance drops the far ones; LOD off draws level 0.
    out.clear();
    cull_batch(b, mesh, planes, view, lod, 1, out);
    REQUIRE(out.size() == 3);
    CHECK(out[0].level == 1);
    CHECK(out[1].level == 2);
    b.cull_distance = 100.0f;
    out.clear();
    cull_batch(b, mesh, planes, view, lod, 0, out);
    CHECK(out.size() == 2);
    b.cull_distance = 0.0f;
    lod.enabled = false;
    out.clear();
    cull_batch(b, mesh, planes, view, lod, 0, out);
    REQUIRE(out.size() == 3);
    for (const BatchVisible& v : out) CHECK(v.level == 0);
}

TEST_CASE("render3d scale: Scene3D takes batches, decals and emitters and counts them") {
    MeshStore store;
    store.init(nullptr);
    const Array<byte> blob = box_with_lods();
    REQUIRE(store.upload(1, 1, blob.span()).has_value());
    Scene3D scene(store, mem::heap());
    scene.begin(camera(), {1280, 720});
    const math::mat4 transforms[] = {math::translation({0.0f, 0.0f, -5.0f}), math::translation({50.0f, 0.0f, -5.0f})};
    InstanceBatch b;
    b.id = 1;
    b.mesh = 1;
    b.transforms = Span<const math::mat4>(transforms, 2);
    scene.add_batch(b);
    InstanceBatch missing = b;  // no such mesh: skipped
    missing.mesh = 9;
    scene.add_batch(missing);
    InstanceBatch empty = b;  // no instances: skipped
    empty.transforms = {};
    scene.add_batch(empty);
    // Decals: one in view, one behind the camera, then more than kMaxDecals in view.
    Decal d;
    d.world = math::translation({0.0f, 0.0f, -5.0f});
    scene.add_decal(d);
    Decal behind;
    behind.world = math::translation({0.0f, 0.0f, 20.0f});
    scene.add_decal(behind);
    for (u32 i = 0; i < kMaxDecals; ++i) scene.add_decal(d);
    // Textured decals past kMaxDecalTextures distinct textures are dropped.
    ParticleEmitter e;
    e.id = 4;
    e.max_particles = kMaxParticlesPerEmitter * 2;  // clamped
    scene.add_particle_emitter(e);
    scene.set_time(1.5f, 0.02f);
    scene.build();
    const Scene3D::Stats& st = scene.stats();
    CHECK(st.batches == 1);
    CHECK(st.batch_instances == 2);
    // Computed bounds: each instance's bounding sphere (the box's half diagonal).
    CHECK(scene.batches()[0].bounds.min.x == doctest::Approx(-0.8660254f));
    CHECK(scene.batches()[0].bounds.max.x == doctest::Approx(50.8660254f));
    CHECK(st.decals == kMaxDecals);
    CHECK(st.decals_culled == 1);
    CHECK(st.decals_dropped == 1);
    CHECK(scene.decals().size() == kMaxDecals);
    CHECK(st.particle_emitters == 1);
    CHECK(scene.particle_emitters()[0].max_particles == kMaxParticlesPerEmitter);
    CHECK(scene.time() == 1.5f);
    CHECK(scene.dt() == 0.02f);

    scene.begin(camera(), {1280, 720});
    for (u32 t = 0; t < kMaxDecalTextures + 1; ++t) {
        Decal td = d;
        td.texture = 100 + t;
        scene.add_decal(td);
    }
    scene.add_decal(d);  // untextured: always fits
    scene.build();
    CHECK(scene.stats().decals == kMaxDecalTextures + 1);
    CHECK(scene.stats().decals_dropped == 1);
    CHECK(scene.decal_textures().size() == kMaxDecalTextures);
    CHECK(scene.batches().empty());
}
