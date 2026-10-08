// The CPU half of the M5a renderer, headless: Forward+ light clustering (sphere screen bounds,
// every point of a light's sphere finds the light in its cluster, determinism, capacity), cascaded
// shadow fits (rotation-invariant slice spheres, whole-texel snapping, contiguous splits, caster
// lists) and node animation (CUBICSPLINE keys, multi-node clips, blending, cross-fades).

#include <doctest/doctest.h>

#include <cmath>

#include "mce/render/clusters.h"
#include "mce/render/mesh.h"
#include "mce/render/scene3d.h"
#include "mesh_fixture.h"

using namespace mce;
using namespace mce::render;
using math::vec3;
using math::vec4;

namespace {

// Deterministic [0, 1) stream (no std random: identical on every platform).
struct Lcg {
    u32 s = 12345;
    f32 next() {
        s = s * 1664525u + 1013904223u;
        return f32(s >> 8) / f32(1u << 24);
    }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * next(); }
};

View3D test_view(f32 yaw = 0.3f) {
    Camera3D cam = orbit_camera({0, 0, 0}, yaw, math::radians(25.0f), 12.0f);
    cam.near_z = 0.1f;
    cam.far_z = 100.0f;
    return compute_view_3d(cam, {320, 180});
}

Array<PointLight> random_lights(u32 n, u32 seed) {
    Lcg rng{seed};
    Array<PointLight> lights;
    for (u32 i = 0; i < n; ++i) {
        PointLight l;
        l.position = {rng.range(-14, 14), rng.range(-1, 4), rng.range(-14, 14)};
        l.radius = rng.range(0.5f, 4.0f);
        l.color = {rng.next(), rng.next(), rng.next()};
        lights.push_back(l);
    }
    return lights;
}

// Cluster a world point falls in, the way the fragment shader finds it.
bool cluster_of(const View3D& v, const ClusterLights& cl, vec3 p, u32& out) {
    const vec4 c = v.view_proj * vec4(p, 1.0f);
    const f32 depth = -math::transform_point(v.view, p).z;
    if (c.w <= 0.0f || depth < v.near_z || depth > v.far_z) return false;
    const f32 nx = c.x / c.w, ny = c.y / c.w;
    if (std::fabs(nx) > 1.0f || std::fabs(ny) > 1.0f) return false;
    const ClusterGridDesc& g = cl.grid();
    const u32 tx = math::min(u32((nx * 0.5f + 0.5f) * f32(g.x)), g.x - 1);
    const u32 ty = math::min(u32((ny * 0.5f + 0.5f) * f32(g.y)), g.y - 1);
    out = cl.cluster_index(tx, ty, cl.slice_of(depth));
    return true;
}

bool lists(const ClusterLights& cl, u32 cluster, u32 light) {
    for (u32 l : cl.lights_in(cluster))
        if (l == light) return true;
    return false;
}

}  // namespace

TEST_CASE("forward+: sphere_ndc_bounds is tight for a sphere ahead and clips at the near plane") {
    f32 lo = 0, hi = 0;
    // On the axis, 10 m out, r = 1: the tangent half-angle is asin(0.1), so ±tan = ±1/sqrt(99).
    REQUIRE(sphere_ndc_bounds(0.0f, 10.0f, 1.0f, 0.1f, 1.0f, lo, hi));
    CHECK(hi == doctest::Approx(1.0f / std::sqrt(99.0f)).epsilon(1e-4));
    CHECK(lo == doctest::Approx(-hi));
    // Off axis: still contains the centre's projection, and the projection scale multiplies.
    REQUIRE(sphere_ndc_bounds(3.0f, 10.0f, 1.0f, 0.1f, 2.0f, lo, hi));
    CHECK(lo < 2.0f * 0.3f);
    CHECK(hi > 2.0f * 0.3f);
    // Entirely behind the near plane.
    CHECK_FALSE(sphere_ndc_bounds(0.0f, -5.0f, 1.0f, 0.1f, 1.0f, lo, hi));
    // The eye inside the sphere: everything.
    REQUIRE(sphere_ndc_bounds(0.0f, 0.5f, 1.0f, 0.1f, 1.0f, lo, hi));
    CHECK(lo < -1e6f);
    CHECK(hi > 1e6f);
    // Crossing the near plane off to the side: the near-plane chord widens the bounds.
    REQUIRE(sphere_ndc_bounds(1.5f, 0.6f, 1.0f, 0.1f, 1.0f, lo, hi));
    CHECK(lo <= (1.5f - std::sqrt(1.0f - 0.25f)) / 0.1f + 1e-3f);
}

TEST_CASE("forward+: every point inside a light's sphere finds the light in its cluster") {
    const View3D v = test_view();
    const Array<PointLight> lights = random_lights(300, 7);
    ClusterLights cl;
    cl.build(v, lights.span());
    CHECK(cl.cluster_count() == 32u * 16u * 24u);
    CHECK(cl.ranges().size() == cl.cluster_count());
    CHECK(cl.lights().size() == lights.size());
    CHECK(cl.stats().lights == 300);
    CHECK(cl.stats().visible > 50);
    CHECK(cl.stats().dropped == 0);
    CHECK(cl.stats().indices == cl.indices().size());

    Lcg rng{99};
    u32 probes = 0;
    for (u32 li = 0; li < lights.size(); ++li) {
        const PointLight& l = lights[li];
        for (int k = 0; k < 40; ++k) {
            const vec3 d{rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)};
            if (math::length_sq(d) > 1.0f) continue;
            const vec3 p = l.position + d * (l.radius * 0.999f);
            u32 c = 0;
            if (!cluster_of(v, cl, p, c)) continue;
            ++probes;
            INFO("light " << li << " probe " << k);
            REQUIRE(lists(cl, c, li));
        }
    }
    CHECK(probes > 1000);

    // Lists are in light order (deterministic shading order), and the pool carries 1/r².
    for (u32 c = 0; c < cl.cluster_count(); ++c) {
        const Span<const u32> ls = cl.lights_in(c);
        for (usize i = 1; i < ls.size(); ++i) REQUIRE(ls[i - 1] < ls[i]);
    }
    CHECK(cl.lights()[3].color.w == doctest::Approx(1.0f / (lights[3].radius * lights[3].radius)));
    CHECK(cl.lights()[3].position_radius.w == doctest::Approx(lights[3].radius));
    CHECK(cl.lights()[3].spot.w == doctest::Approx(1.0f));  // a point light: the cone term is 1

    // A spot cone: 1 inside the inner angle, 0 at the outer one, on the GPU's formula.
    Array<PointLight> spots;
    PointLight sp;
    sp.direction = {0, -2, 0};  // normalized on use
    sp.spot_inner = math::radians(10.0f);
    sp.spot_outer = math::radians(30.0f);
    spots.push_back(sp);
    ClusterLights cs;
    cs.build(v, spots.span());
    const vec4 k = cs.lights()[0].spot;
    const auto cone = [&](f32 angle) {
        const vec3 to_light{-std::sin(angle), std::cos(angle), 0.0f};  // L: surface -> light
        return math::clamp(math::dot(-to_light, vec3{k.x, k.y, k.z}) + k.w, 0.0f, 1.0f);
    };
    CHECK(cone(0.0f) == doctest::Approx(1.0f));
    CHECK(cone(math::radians(9.0f)) == doctest::Approx(1.0f));
    CHECK(cone(math::radians(30.0f)) == doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(cone(math::radians(20.0f)) > 0.2f);
    CHECK(cone(math::radians(20.0f)) < 0.8f);
}

TEST_CASE("forward+: no light lands in a cluster its sphere cannot touch") {
    const View3D v = test_view(1.1f);
    const Array<PointLight> lights = random_lights(64, 3);
    ClusterLights cl;
    cl.build(v, lights.span());
    // A cluster's centre ray at mid-slice depth is inside the cluster; a light whose sphere is
    // farther from that point than the cluster's own diagonal cannot touch it.
    const ClusterGridDesc& g = cl.grid();
    const f32 px = v.proj.c[0][0], py = v.proj.c[1][1];
    u32 checked = 0;
    for (u32 s = 0; s < g.z; ++s) {
        const f32 dn = std::exp((f32(s) - cl.slice_bias()) / cl.slice_scale());
        const f32 df = std::exp((f32(s + 1) - cl.slice_bias()) / cl.slice_scale());
        for (u32 ty = 0; ty < g.y; ++ty)
            for (u32 tx = 0; tx < g.x; ++tx) {
                const f32 nx0 = f32(tx) / f32(g.x) * 2 - 1, nx1 = f32(tx + 1) / f32(g.x) * 2 - 1;
                const f32 ny0 = f32(ty) / f32(g.y) * 2 - 1, ny1 = f32(ty + 1) / f32(g.y) * 2 - 1;
                const vec3 lo{math::min(nx0 * dn, nx0 * df) / px, math::min(ny0 * dn, ny0 * df) / py, -df};
                const vec3 hi{math::max(nx1 * dn, nx1 * df) / px, math::max(ny1 * dn, ny1 * df) / py, -dn};
                for (u32 li : cl.lights_in(cl.cluster_index(tx, ty, s))) {
                    const vec3 c = math::transform_point(v.view, lights[li].position);
                    const vec3 q{math::clamp(c.x, lo.x, hi.x), math::clamp(c.y, lo.y, hi.y), math::clamp(c.z, lo.z, hi.z)};
                    REQUIRE(math::length_sq(c - q) <= lights[li].radius * lights[li].radius * 1.0001f + 1e-5f);
                    ++checked;
                }
            }
    }
    CHECK(checked == cl.indices().size());
}

TEST_CASE("forward+: the build is a pure function; capacity overflow is counted, not written") {
    const View3D v = test_view();
    const Array<PointLight> lights = random_lights(256, 21);
    ClusterLights a, b;
    a.build(v, lights.span());
    b.build(v, lights.span());
    REQUIRE(a.indices().size() == b.indices().size());
    for (usize i = 0; i < a.indices().size(); ++i) REQUIRE(a.indices()[i] == b.indices()[i]);
    for (usize i = 0; i < a.ranges().size(); ++i) REQUIRE(a.ranges()[i].offset == b.ranges()[i].offset);

    ClusterGridDesc small;
    small.max_indices = 100;
    ClusterLights c;
    c.build(v, lights.span(), small);
    CHECK(c.indices().size() == 100);
    CHECK(c.stats().dropped == a.indices().size() - 100);

    // Lights that are dark or behind the camera touch nothing.
    Array<PointLight> off;
    PointLight dark;
    dark.intensity = 0.0f;
    off.push_back(dark);
    PointLight behind;
    behind.position = v.position - v.forward * 10.0f;
    behind.radius = 1.0f;
    off.push_back(behind);
    ClusterLights d;
    d.build(v, off.span());
    CHECK(d.indices().empty());
    CHECK(d.stats().visible == 0);

    // A light around the eye is listed in every cluster of the nearest slice.
    Array<PointLight> eye;
    PointLight e;
    e.position = v.position;
    e.radius = 0.5f;
    eye.push_back(e);
    ClusterLights f;
    f.build(v, eye.span());
    CHECK(f.lights_in(f.cluster_index(0, 0, 0)).size() == 1);
    CHECK(f.lights_in(f.cluster_index(31, 15, 0)).size() == 1);
    CHECK(f.lights_in(f.cluster_index(5, 5, 23)).empty());
}

TEST_CASE("csm: the slice sphere ignores camera rotation and bounds the slice corners") {
    for (f32 yaw : {0.0f, 0.7f, 2.4f}) {
        const View3D v = test_view(yaw);
        f32 cd = 0, r = 0;
        frustum_slice_sphere(v, 2.0f, 10.0f, cd, r);
        const View3D ref = test_view(0.0f);
        f32 cd0 = 0, r0 = 0;
        frustum_slice_sphere(ref, 2.0f, 10.0f, cd0, r0);
        CHECK(cd == doctest::Approx(cd0));
        CHECK(r == doctest::Approx(r0));
        // All 8 corners of the slice are inside the sphere.
        const f32 tx = 1.0f / v.proj.c[0][0], ty = 1.0f / v.proj.c[1][1];
        for (f32 d : {2.0f, 10.0f})
            for (f32 sx : {-1.0f, 1.0f})
                for (f32 sy : {-1.0f, 1.0f}) {
                    const vec3 corner{sx * tx * d, sy * ty * d, -d};
                    CHECK(math::length(corner - vec3{0, 0, -cd}) <= r * 1.0001f);
                }
    }
}

TEST_CASE("csm: cascades snap to whole texels as the camera moves (no shimmer)") {
    const vec3 sun = math::normalize(vec3{-0.5f, -1.0f, -0.3f});
    const math::aabb3 scene{{-20, -1, -20}, {20, 5, 20}};
    f32 frac0 = 0, fracy0 = 0, r0 = 0;
    for (int step = 0; step < 12; ++step) {
        Camera3D cam = orbit_camera({0.137f * f32(step), 0, -0.071f * f32(step)}, 0.05f * f32(step), 0.4f, 10.0f);
        cam.far_z = 100.0f;
        const View3D v = compute_view_3d(cam, {320, 180});
        Cascade c;
        fit_cascade(v, sun, 0.1f, 12.0f, 1024, scene, c);
        // A fixed world point lands on the same sub-texel offset every frame.
        const vec4 p = c.view_proj * vec4(vec3{1.234f, 0.0f, -2.5f}, 1.0f);
        const f32 tx = (p.x * 0.5f + 0.5f) * 1024.0f, ty = (p.y * 0.5f + 0.5f) * 1024.0f;
        const f32 fx = tx - std::floor(tx), fy = ty - std::floor(ty);
        if (step == 0) {
            frac0 = fx;
            fracy0 = fy;
            r0 = c.radius;
        } else {
            CHECK(std::fabs(fx - frac0) < 0.02f);
            CHECK(std::fabs(fy - fracy0) < 0.02f);
            CHECK(c.radius == r0);  // the size never changes with rotation either
        }
        CHECK(c.texel_world == doctest::Approx(2.0f * c.radius / 1024.0f));
        // The whole scene is inside the stretched depth range (casters behind the slice count).
        for (int k = 0; k < 8; ++k) {
            const vec3 corner{k & 1 ? 20.0f : -20.0f, k & 2 ? 5.0f : -1.0f, k & 4 ? 20.0f : -20.0f};
            const vec4 q = c.view_proj * vec4(corner, 1.0f);
            CHECK(q.z >= -1.0001f);
        }
    }
}

TEST_CASE("csm: Scene3D splits the shadow distance into contiguous cascades with caster lists") {
    MeshStore store;
    mesh_fixture::Builder b;
    b.add_node(-1);
    mesh_fixture::Box box;
    box.half = {0.5f, 0.5f, 0.5f};
    b.add_box(box);
    const Array<byte> blob = b.build();
    REQUIRE(store.upload(1, 1, blob.span()).has_value());

    Scene3D scene(store, mem::heap());
    Camera3D cam;
    cam.position = {0, 2, 0};
    cam.far_z = 200.0f;
    scene.begin(cam, {256, 144});
    ShadowSettings ss;
    ss.cascades = 3;
    ss.distance = 60.0f;
    ss.splits[0] = 0.1f;
    ss.splits[1] = 0.3f;
    scene.set_shadows(ss);
    for (int i = 0; i < 20; ++i) {  // a line of boxes marching away from the camera
        MeshInstance m;
        m.mesh = 1;
        m.world = math::translation({0, 0, -2.0f - 3.0f * f32(i)});
        scene.add(m);
    }
    MeshInstance no_cast;
    no_cast.mesh = 1;
    no_cast.world = math::translation({1, 0, -3});
    no_cast.cast_shadows = false;
    scene.add(no_cast);
    scene.build();

    const Span<const Cascade> cs = scene.cascades();
    REQUIRE(cs.size() == 3);
    CHECK(cs[0].near_depth == doctest::Approx(0.1f));
    CHECK(cs[0].far_depth == doctest::Approx(6.0f));
    CHECK(cs[1].near_depth == doctest::Approx(6.0f));
    CHECK(cs[1].far_depth == doctest::Approx(18.0f));
    CHECK(cs[2].near_depth == doctest::Approx(18.0f));
    CHECK(cs[2].far_depth == doctest::Approx(60.0f));
    CHECK(cs[0].radius < cs[1].radius);
    CHECK(cs[1].radius < cs[2].radius);
    CHECK(scene.shadow_draws().size() == 20);  // the non-caster is only in the main pass
    // Near boxes are in cascade 0's list, the farthest ones only in the last cascade.
    u32 total = 0;
    for (u32 i = 0; i < 3; ++i) {
        CHECK(cs[i].caster_count > 0);
        CHECK(cs[i].caster_count <= 20);
        CHECK(scene.stats().cascade_casters[i] == cs[i].caster_count);
        total += cs[i].caster_count;
    }
    CHECK(scene.cascade_casters().size() == total);
    CHECK(cs[0].caster_count < 10);  // the slice sphere of [0.1, 6] m reaches only the first few
    CHECK(cs[0].caster_count < cs[2].caster_count);

    // Shadows off: no cascades and no casters.
    scene.begin(cam, {256, 144});
    DirectionalLight l;
    l.shadows = false;
    scene.set_light(l);
    MeshInstance m;
    m.mesh = 1;
    scene.add(m);
    scene.build();
    CHECK(scene.cascades().empty());
    CHECK(scene.cascade_casters().empty());
}

TEST_CASE("anim: CUBICSPLINE keys interpolate as a Hermite curve; clips drive several nodes") {
    mesh_fixture::Builder b;
    const u32 root = b.add_node(-1);
    const u32 a = b.add_node(static_cast<i32>(root));
    const u32 c = b.add_node(static_cast<i32>(root));
    mesh_fixture::Box box;
    box.half = {0.1f, 0.1f, 0.1f};
    box.node = a;
    b.add_box(box);
    box.node = c;
    b.add_box(box);

    asset::MeshClip clip{};
    clip.name_offset = 0;
    for (char ch : StringView("move")) b.strings.push_back(ch);
    clip.name_size = 4;
    clip.duration = 2.0f;
    clip.first_channel = 0;
    clip.channel_count = 2;
    // Node a: cubic translation along X, keys at t = 0 and 2: value 0 -> 4, out-tangent 3 at key 0,
    // in-tangent -1 at key 1 (glTF tangents are per second: scaled by the key interval).
    asset::MeshChannel ch{};
    ch.node = a;
    ch.path = asset::AnimPath::Translation;
    ch.interp = asset::AnimInterp::CubicSpline;
    ch.key_count = 2;
    ch.times_offset = static_cast<u32>(b.keys.size());
    b.keys.push_back(0.0f);
    b.keys.push_back(2.0f);
    ch.values_offset = static_cast<u32>(b.keys.size());
    const f32 cubic[2][3][3] = {{{0, 0, 0}, {0, 0, 0}, {3, 0, 0}}, {{-1, 0, 0}, {4, 0, 0}, {0, 0, 0}}};
    for (const auto& key : cubic)
        for (const auto& v : key)
            for (f32 x : v) b.keys.push_back(x);
    b.channels.push_back(ch);
    // Node c: a STEP scale track (1 until t = 1, then 2).
    asset::MeshChannel st{};
    st.node = c;
    st.path = asset::AnimPath::Scale;
    st.interp = asset::AnimInterp::Step;
    st.key_count = 2;
    st.times_offset = static_cast<u32>(b.keys.size());
    b.keys.push_back(0.0f);
    b.keys.push_back(1.0f);
    st.values_offset = static_cast<u32>(b.keys.size());
    for (f32 x : {1.0f, 1.0f, 1.0f, 2.0f, 2.0f, 2.0f}) b.keys.push_back(x);
    b.channels.push_back(st);
    b.clips.push_back(clip);

    const Array<byte> blob = b.build();
    MeshStore store;
    REQUIRE(store.upload(1, 1, blob.span()).has_value());
    const GpuMesh& m = *store.get(1);
    REQUIRE(m.find_clip("move") == 0);

    math::mat4 pose[3];
    const Span<math::mat4> out(pose, 3);
    pose_nodes(m, 0, 0.0f, out);
    CHECK(math::transform_point(pose[a], {}).x == doctest::Approx(0.0f));
    CHECK(math::transform_point(pose[c], {1, 0, 0}).x == doctest::Approx(1.0f));
    // Hermite at s = 0.5 over dt = 2: 0.5 p0 + 0.125 dt b0 + 0.5 p1 - 0.125 dt a1 = 0 + 0.75 + 2 + 0.25.
    pose_nodes(m, 0, 1.0f - 1e-4f, out);
    CHECK(math::transform_point(pose[c], {1, 0, 0}).x == doctest::Approx(1.0f));  // step holds
    pose_nodes(m, 0, 1.0f, out);
    CHECK(math::transform_point(pose[a], {}).x == doctest::Approx(3.0f).epsilon(1e-4));
    CHECK(math::transform_point(pose[c], {1, 0, 0}).x == doctest::Approx(2.0f));
    pose_nodes(m, 0, 1.999f, out);
    CHECK(math::transform_point(pose[a], {}).x == doctest::Approx(4.0f).epsilon(1e-2));
}

TEST_CASE("anim: blended poses and AnimationPlayer cross-fades have no pop") {
    mesh_fixture::Builder b;
    const u32 root = b.add_node(-1);
    const u32 child = b.add_node(static_cast<i32>(root), {2, 0, 0});
    mesh_fixture::Box box;
    box.half = {0.1f, 0.1f, 0.1f};
    box.node = child;
    b.add_box(box);
    b.add_spin_clip(root, 4.0f, "spin");
    b.add_spin_clip(root, 8.0f, "slow");
    const Array<byte> blob = b.build();
    MeshStore store;
    REQUIRE(store.upload(1, 1, blob.span()).has_value());
    const GpuMesh& m = *store.get(1);

    math::mat4 single[2], blended[2];
    // A single full-weight sample is pose_nodes.
    pose_nodes(m, 0, 1.0f, Span<math::mat4>(single, 2));
    const ClipSample one[] = {{0, 1.0f, 1.0f}};
    pose_nodes_blend(m, Span<const ClipSample>(one, 1), Span<math::mat4>(blended, 2));
    CHECK(math::length(math::transform_point(single[child], {}) - math::transform_point(blended[child], {})) < 1e-4f);
    // Half rest (0°), half a quarter turn (90°): 45°, still 2 m out (nlerp keeps the length).
    const ClipSample half[] = {{~0u, 0.0f, 1.0f}, {0, 1.0f, 1.0f}};
    pose_nodes_blend(m, Span<const ClipSample>(half, 2), Span<math::mat4>(blended, 2));
    const vec3 p = math::transform_point(blended[child], {});
    CHECK(p.x == doctest::Approx(2.0f * 0.70710678f).epsilon(1e-3));
    CHECK(p.z == doctest::Approx(-2.0f * 0.70710678f).epsilon(1e-3));
    // All-zero weights: the rest pose.
    const ClipSample none[] = {{0, 1.0f, 0.0f}};
    pose_nodes_blend(m, Span<const ClipSample>(none, 1), Span<math::mat4>(blended, 2));
    CHECK(math::transform_point(blended[child], {}).x == doctest::Approx(2.0f));

    AnimationPlayer player;
    player.play(0);
    player.advance(1.0f);
    CHECK(player.clip() == 0);
    CHECK(player.time() == doctest::Approx(1.0f));
    CHECK(player.blend() == doctest::Approx(1.0f));
    math::mat4 before[2], after[2];
    player.pose(m, Span<math::mat4>(before, 2));
    player.play(1, 0.5f);  // fade to "slow" over 0.5 s
    player.pose(m, Span<math::mat4>(after, 2));
    // The switch itself does not move anything.
    CHECK(math::length(math::transform_point(before[child], {}) - math::transform_point(after[child], {})) < 1e-4f);
    player.advance(0.25f);
    CHECK(player.clip() == 1);
    CHECK(player.blend() == doctest::Approx(0.5f));
    player.advance(0.5f);
    CHECK(player.blend() == doctest::Approx(1.0f));
    player.pose(m, Span<math::mat4>(after, 2));
    pose_nodes(m, 1, 0.75f, Span<math::mat4>(single, 2));
    CHECK(math::length(math::transform_point(single[child], {}) - math::transform_point(after[child], {})) < 1e-4f);
}
