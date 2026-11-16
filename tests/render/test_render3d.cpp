// The CPU half of the MVP 3D path, headless: MeshStore (no device), node animation sampling,
// frustum culling (FR-R3D-050: off-screen instances issue no draws), 64-bit key sort order and
// its determinism, and the shadow fit.

#include <doctest/doctest.h>

#include "mce/render/mesh.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::render;
using math::vec3;

namespace {

Array<byte> one_box_mesh(u32 flags = 0) {
    mesh_fixture::Builder b;
    b.add_node(-1);
    mesh_fixture::Box box;
    box.half = {0.5f, 0.5f, 0.5f};
    box.flags = flags;
    b.add_box(box);
    return b.build();
}

MeshInstance at(u32 mesh, vec3 p) {
    MeshInstance i;
    i.mesh = mesh;
    i.world = math::translation(p);
    return i;
}

Camera3D looking_down_minus_z() {
    Camera3D c;  // at the origin, looking down -Z
    c.near_z = 0.1f;
    c.far_z = 200.0f;
    return c;
}

}  // namespace

TEST_CASE("render3d: MeshStore keeps the CPU side without a device; generations replace") {
    MeshStore store;
    store.init(nullptr);
    const Array<byte> blob = one_box_mesh();
    REQUIRE(store.upload(7, 1, blob.span()).has_value());
    const GpuMesh* m = store.get(7);
    REQUIRE(m);
    CHECK(m->triangle_count() == 12);
    CHECK(m->submeshes.size() == 1);
    CHECK(m->vertices.is_null());
    CHECK(m->bounds.max.x == doctest::Approx(0.5f));
    CHECK(m->submeshes[0].albedo_key == ~0u);

    REQUIRE(store.upload(7, 2, blob.span()).has_value());
    CHECK(store.get(7)->generation == 2);
    store.retire(7, 2);
    CHECK(store.get(7)->generation == 1);
    store.release(7);
    CHECK(store.get(7) == nullptr);
    CHECK_FALSE(store.upload(8, 1, blob.span().subspan(0, 100)).has_value());
    CHECK(store.get(8) == nullptr);
}

TEST_CASE("render3d: pose_nodes samples a node rotation clip and composes parents") {
    mesh_fixture::Builder b;
    const u32 root = b.add_node(-1, {0, 1, 0});
    const u32 child = b.add_node(static_cast<i32>(root), {2, 0, 0});
    mesh_fixture::Box box;
    box.half = {0.1f, 0.1f, 0.1f};
    box.node = child;
    b.add_box(box);
    b.add_spin_clip(root, 4.0f);
    const Array<byte> blob = b.build();
    MeshStore store;
    REQUIRE(store.upload(1, 1, blob.span()).has_value());
    const GpuMesh& m = *store.get(1);
    REQUIRE(m.find_clip("spin") == 0);
    CHECK(m.find_clip("nope") == m.clips.size());

    math::mat4 pose[2];
    pose_nodes(m, 0, 1.0f, Span<math::mat4>(pose, 2));  // a quarter turn about +Y
    const vec3 p = math::transform_point(pose[child], {0, 0, 0});
    CHECK(p.x == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(p.y == doctest::Approx(1.0f));
    CHECK(p.z == doctest::Approx(-2.0f));
    pose_nodes(m, 0, 4.5f, Span<math::mat4>(pose, 2));  // loops: same as t = 0.5 (an eighth)
    const vec3 q = math::transform_point(pose[child], {0, 0, 0});
    CHECK(q.x == doctest::Approx(2.0f * 0.70710678f).epsilon(1e-3));
    pose_nodes(m, 99, 1.0f, Span<math::mat4>(pose, 2));  // no such clip: rest pose
    CHECK(math::transform_point(pose[child], {0, 0, 0}).x == doctest::Approx(2.0f));
}

TEST_CASE("render3d: frustum culling drops off-screen instances (FR-R3D-050)") {
    MeshStore store;
    const Array<byte> blob = one_box_mesh();
    REQUIRE(store.upload(1, 1, blob.span()).has_value());
    Scene3D scene(store, mem::heap());
    scene.begin(looking_down_minus_z(), {256, 256});
    for (int i = 0; i < 10; ++i) {
        scene.add(at(1, {f32(i % 3) - 1.0f, 0.0f, -5.0f - f32(i)}));  // in front
        scene.add(at(1, {0.0f, 0.0f, 5.0f + f32(i)}));                // behind the camera
    }
    scene.add(at(99, {0, 0, -5}));  // not resident: skipped
    scene.build();
    CHECK(scene.stats().instances == 20);
    CHECK(scene.stats().submeshes == 20);
    CHECK(scene.stats().visible == 10);
    CHECK(scene.draws().size() == 10);
    CHECK(scene.stats().triangles == 120);
    CHECK(scene.shadow_draws().size() == 20);  // shadows come from off-screen casters too
}

TEST_CASE("render3d: opaque draws sort front-to-back, blended back-to-front after them") {
    MeshStore store;
    REQUIRE(store.upload(1, 1, one_box_mesh().span()).has_value());
    REQUIRE(store.upload(2, 1, one_box_mesh(asset::MaterialFlags::AlphaBlend).span()).has_value());
    Scene3D scene(store, mem::heap());
    scene.begin(looking_down_minus_z(), {256, 256});
    scene.add(at(1, {0, 0, -15}));
    scene.add(at(2, {0, 0, -6}));
    scene.add(at(1, {0, 0, -5}));
    scene.add(at(2, {0, 0, -20}));
    scene.add(at(1, {0, 0, -10}));
    scene.build();
    REQUIRE(scene.draws().size() == 5);
    CHECK(scene.opaque_count() == 3);
    const u32 expect[] = {2, 4, 0, 3, 1};  // opaque by depth ascending, then blended descending
    for (u32 i = 0; i < 5; ++i) CHECK(scene.draws()[i].instance == expect[i]);
    CHECK(scene.shadow_draws().size() == 3);  // blended submeshes cast no shadow

    // Same input, same order (FR-RUN-040: draw order is a function of scene + camera).
    Scene3D again(store, mem::heap());
    again.begin(looking_down_minus_z(), {256, 256});
    for (const MeshInstance& i : scene.instances()) again.add(i);
    again.build();
    for (u32 i = 0; i < 5; ++i) CHECK(again.draws()[i].instance == scene.draws()[i].instance);
}

TEST_CASE("render3d: reflection probes draw every opaque submesh, culled or not; at most four") {
    MeshStore store;
    REQUIRE(store.upload(1, 1, one_box_mesh().span()).has_value());
    REQUIRE(store.upload(2, 1, one_box_mesh(asset::MaterialFlags::AlphaBlend).span()).has_value());
    Scene3D scene(store, mem::heap());
    scene.begin(looking_down_minus_z(), {256, 256});
    scene.add(at(1, {0, 0, -5}));
    scene.add(at(1, {0, 0, 5}));   // behind the camera: no main draw, still in the captures
    scene.add(at(2, {0, 0, -6}));  // blended: not captured
    scene.build();
    CHECK(scene.probe_draws().empty());  // no probes, no capture list

    scene.begin(looking_down_minus_z(), {256, 256});
    scene.add(at(1, {0, 0, -5}));
    scene.add(at(1, {0, 0, 5}));
    scene.add(at(2, {0, 0, -6}));
    for (u32 i = 0; i < kMaxReflectionProbes + 2; ++i) scene.add_reflection_probe({});
    scene.build();
    CHECK(scene.draws().size() == 2);
    CHECK(scene.probe_draws().size() == 2);
    CHECK(scene.reflection_probes().size() == kMaxReflectionProbes);
    CHECK(scene.stats().probes_dropped == 2);
}

TEST_CASE("render3d: equal keys keep submission order") {
    MeshStore store;
    REQUIRE(store.upload(1, 1, one_box_mesh().span()).has_value());
    Scene3D scene(store, mem::heap());
    scene.begin(looking_down_minus_z(), {256, 256});
    for (int i = 0; i < 6; ++i) scene.add(at(1, {f32(i) * 0.0f, 0, -8}));
    scene.build();
    for (u32 i = 0; i < 6; ++i) CHECK(scene.draws()[i].instance == i);
}

TEST_CASE("render3d: the shadow fit contains the scene; transform_aabb is exact for rotations") {
    MeshStore store;
    REQUIRE(store.upload(1, 1, one_box_mesh().span()).has_value());
    Scene3D scene(store, mem::heap());
    scene.begin(orbit_camera({0, 0, 0}, 0.3f, 0.5f, 12.0f), {320, 200});
    scene.add(at(1, {-4, 0, 3}));
    scene.add(at(1, {5, 2, -1}));
    scene.build();
    const math::aabb3& b = scene.scene_bounds();
    CHECK(b.min.x == doctest::Approx(-4.5f));
    CHECK(b.max.y == doctest::Approx(2.5f));
    for (int c = 0; c < 8; ++c) {
        const vec3 p{(c & 1) ? b.max.x : b.min.x, (c & 2) ? b.max.y : b.min.y, (c & 4) ? b.max.z : b.min.z};
        const math::vec4 s = scene.light_view_proj() * math::vec4(p, 1.0f);
        CHECK(std::fabs(s.x / s.w) <= 1.0f);
        CHECK(std::fabs(s.y / s.w) <= 1.0f);
        CHECK(std::fabs(s.z / s.w) <= 1.0f);
    }
    const math::mat4 r = math::rotation(math::quat_from_axis_angle({0, 1, 0}, math::kPi * 0.25f));
    const math::aabb3 rb = transform_aabb(r, {{-1, -1, -1}, {1, 1, 1}});
    CHECK(rb.max.x == doctest::Approx(1.41421356f));
    CHECK(rb.max.y == doctest::Approx(1.0f));

    // An orbit camera at yaw 0 / pitch 0 sits on +Z looking at the target.
    const Camera3D c = orbit_camera({1, 2, 3}, 0.0f, 0.0f, 10.0f);
    CHECK(c.position.z == doctest::Approx(13.0f));
    const View3D v = compute_view_3d(c, {100, 100});
    CHECK(sphere_visible(v, {1, 2, 3}, 0.1f));
    CHECK_FALSE(sphere_visible(v, {1, 2, 20}, 0.1f));
}

TEST_CASE("render3d: Atmosphere::sky_sun_direction lights the sky in place of the light, which keeps its own tint") {
    MeshStore store;
    auto built = [&](const DirectionalLight& light, const Atmosphere& a, vec3& sun, vec3& irr, DirectionalLight& out) {
        Scene3D scene(store, mem::heap());
        scene.begin(looking_down_minus_z(), {64, 64});
        scene.set_light(light);
        scene.set_atmosphere(a);
        scene.build();
        sun = scene.atmosphere_sun();
        irr = scene.sun_irradiance();
        out = scene.light();
    };
    DirectionalLight moon;  // a moon high in the east holds the light
    moon.direction = math::normalize(vec3{-0.5f, -0.8f, 0.0f});
    moon.color = {0.6f, 0.7f, 1.0f};
    moon.intensity = 0.1f;
    Atmosphere a;
    a.enabled = true;
    vec3 sun_default, irr_default, sun_own, irr_own;
    DirectionalLight lit_default, lit_own;
    built(moon, a, sun_default, irr_default, lit_default);  // unset: the light is the sky's sun, as before
    CHECK(sun_default.y > 0.5f);
    CHECK(irr_default.z == doctest::Approx(1.0f * 0.1f * 3.14159265f).epsilon(1e-4));
    a.sky_sun_direction = math::normalize(vec3{0.9f, 0.1f, 0.0f});  // the sun just under the western horizon
    a.sky_sun_light = {2.0f, 1.0f, 0.5f};
    built(moon, a, sun_own, irr_own, lit_own);
    CHECK(sun_own.y < 0.0f);
    CHECK(sun_own.x < -0.9f);
    CHECK(irr_own.x == doctest::Approx(2.0f * 3.14159265f).epsilon(1e-4));
    CHECK(irr_own.z == doctest::Approx(0.5f * 3.14159265f).epsilon(1e-4));
    // The moonlight is tinted along its own (high) direction either way: the same colour.
    CHECK(lit_own.color.x == doctest::Approx(lit_default.color.x));
    CHECK(lit_own.color.z == doctest::Approx(lit_default.color.z));
    CHECK(lit_own.color.z > 0.5f);
}
