// The editor orbit camera and its collision gate (ROADMAP M5b, ADR-0029 §3), headless: orbit /
// pan / dolly / frame math, the boom cut by World3D::camera_sweep against the scene's colliders
// (extruded 2D shapes and kit3d boxes), the world following document edits, the wireframe
// projection, and framing a kit3d scene through its camera.

#include <doctest/doctest.h>

#include <cmath>

#include "mce/ecs/transform.h"
#include "mce/editor/orbit_camera.h"
#include "mce/kit/kit3d.h"
#include "mce/physics/components.h"

using namespace mce;
using namespace mce::editor;
using namespace mce::math;

namespace {

EntityId add_box(EditorScene& s, const char* name, vec2 pos, vec2 half) {
    const EntityId id = s.create_entity(name);
    LocalTransform t;
    t.position = {pos.x, pos.y, 0.0f};
    s.set(id, t);
    s.add_component(id, refl::type_of<physics::Collider2D>());
    physics::Collider2D c = *s.get<physics::Collider2D>(id);
    c.shape.half = half;
    s.set(id, c);
    return id;
}

bool close(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

}  // namespace

TEST_CASE("orbit camera: orbit, pan, dolly and frame") {
    OrbitCamera cam;
    cam.target = {1, 2, 3};
    cam.yaw = 0.0f;
    cam.pitch = 0.0f;
    cam.distance = cam.effective = 10.0f;
    CHECK(close(cam.eye().x, 1.0f));
    CHECK(close(cam.eye().y, 2.0f));
    CHECK(close(cam.eye().z, 13.0f));  // yaw 0 looks down -Z: the eye sits on +Z
    const render::Camera3D c = cam.camera();
    CHECK(close(c.position.z, 13.0f));

    cam.orbit({0.0f, 1e6f});  // pitch clamps short of straight down
    CHECK(cam.pitch < kPi * 0.5f);
    CHECK(cam.eye().y > 2.0f);
    cam.orbit({0.0f, -2e6f});
    CHECK(cam.pitch > -kPi * 0.5f);

    cam.pitch = 0.0f;
    const vec3 before = cam.target;
    cam.pan({100.0f, 0.0f}, {800, 600});  // drag right: the target moves left (scene follows the cursor)
    CHECK(cam.target.x < before.x);
    CHECK(close(cam.target.y, before.y));

    cam.dolly(1.0f);
    CHECK(close(cam.distance, 8.5f));
    cam.dolly(-1000.0f);
    CHECK(cam.distance == cam.max_distance);
    cam.dolly(1000.0f);
    CHECK(cam.distance == cam.min_distance);

    cam.frame(aabb3{{-2, -1, -0.5f}, {2, 1, 0.5f}});
    CHECK(close(cam.target.x, 0.0f));
    CHECK(cam.distance > 2.0f);
    CHECK(cam.effective == cam.distance);
}

TEST_CASE("orbit camera: the boom stops in front of a wall and above the floor, then slides back out") {
    EditorScene s;
    add_box(s, "Ground", {0, -1}, {10, 0.5f});
    const EntityId wall = add_box(s, "Wall", {-3, 2}, {0.5f, 3});
    OrbitColliders colliders;
    colliders.sync(s, 20.0f);
    CHECK(colliders.body_count() == 2);
    CHECK(close(colliders.bounds().min.x, -10.0f));
    CHECK(close(colliders.bounds().max.z, 10.0f));

    OrbitCamera cam;
    cam.target = {0, 1, 0};
    cam.distance = 8.0f;
    cam.probe_radius = 0.2f;

    // Looking down from above: nothing between the target and the eye.
    cam.yaw = 0.0f;
    cam.pitch = 0.6f;
    CHECK_FALSE(cam.resolve(colliders.world()));
    CHECK(cam.effective == cam.distance);

    // Looking up from below: the boom goes through the ground and is cut above its top face.
    cam.pitch = -0.6f;
    CHECK(cam.resolve(colliders.world()));
    CHECK(cam.effective < cam.distance);
    CHECK(cam.eye().y >= -0.5f + 0.15f);  // ground top at y = -0.5, probe radius 0.2

    // Eye to the -X side: the wall at x = -3 (faces at -3.5 / -2.5) stops the boom in front of it.
    cam.pitch = 0.0f;
    cam.yaw = -kPi * 0.5f;
    CHECK(close(cam.back().x, -1.0f));
    CHECK(cam.resolve(colliders.world()));
    CHECK(cam.eye().x > -2.5f);
    CHECK(close(cam.eye().x, -2.3f, 0.05f));

    // The world follows the document: move the wall away and the boom is whole again.
    LocalTransform t = *s.get<LocalTransform>(wall);
    t.position.x = -30.0f;
    s.set(wall, t);
    colliders.sync(s, 20.0f);
    CHECK_FALSE(cam.resolve(colliders.world()));
    CHECK(cam.effective == cam.distance);
    s.set(wall, *s.get<LocalTransform>(wall));  // unchanged: no rebuild, same answer
    t.position.x = -3.0f;
    s.set(wall, t);
    colliders.sync(s, 20.0f);
    CHECK(cam.resolve(colliders.world()));
    s.destroy_entity(wall);
    colliders.sync(s, 20.0f);
    CHECK(colliders.body_count() == 1);
    CHECK_FALSE(cam.resolve(colliders.world()));

    // Sensors never stop the camera.
    const EntityId trigger = add_box(s, "Trigger", {-3, 1}, {0.5f, 3});
    physics::Collider2D c = *s.get<physics::Collider2D>(trigger);
    c.is_sensor = true;
    s.set(trigger, c);
    colliders.sync(s, 20.0f);
    CHECK(colliders.body_count() == 1);
    CHECK_FALSE(cam.resolve(colliders.world()));
}

TEST_CASE("orbit camera: wireframe segments project to viewport pixels, clipped at the near plane") {
    OrbitCamera cam;
    cam.target = {0, 0, 0};
    cam.yaw = 0.0f;
    cam.pitch = 0.0f;
    cam.distance = cam.effective = 10.0f;
    const render::View3D view = render::compute_view_3d(cam.camera(), {800, 600});
    vec2 a, b;
    REQUIRE(project_segment(view, Segment3D{{0, 0, 0}, {1, 0, 0}}, a, b));
    CHECK(close(a.x, 400.0f, 0.5f));
    CHECK(close(a.y, 300.0f, 0.5f));
    CHECK(b.x > a.x);  // +X is to the right
    REQUIRE(project_segment(view, Segment3D{{0, 0, 0}, {0, 1, 0}}, a, b));
    CHECK(b.y < a.y);  // +Y is up (pixel y grows down)
    CHECK_FALSE(project_segment(view, Segment3D{{0, 0, 20}, {1, 0, 30}}, a, b));  // behind the eye
    CHECK(project_segment(view, Segment3D{{0, 0, 0}, {0, 0, 30}}, a, b));         // crosses the near plane

    EditorScene s;
    add_box(s, "Box", {0, 0}, {1, 1});
    Array<Segment3D> segs;
    orbit_wireframe(s, 1.0f, segs);
    CHECK(segs.size() == 12 + 4 + 3);  // box edges, the collider's quad, the axes
}

TEST_CASE("orbit camera: kit3d boxes collide and a 3D scene frames through its camera") {
    EditorScene s;
    // A floor and a wall as Room3D authors them: the box offset up from the entity, a yawed wall.
    const EntityId floor = s.create_entity("Floor");
    s.set(floor, LocalTransform{});
    s.add_component(floor, refl::type_of<kit::BoxCollider3D>());
    kit::BoxCollider3D fb = *s.get<kit::BoxCollider3D>(floor);
    fb.half = {10.0f, 0.25f, 10.0f};
    fb.offset = {0.0f, -0.25f, 0.0f};
    s.set(floor, fb);
    const EntityId wall = s.create_entity("Wall");
    LocalTransform wt;
    wt.position = {0.0f, 0.0f, -6.0f};
    s.set(wall, wt);
    s.add_component(wall, refl::type_of<kit::BoxCollider3D>());
    kit::BoxCollider3D wb = *s.get<kit::BoxCollider3D>(wall);
    wb.half = {6.0f, 1.5f, 0.25f};
    wb.offset = {0.0f, 1.5f, 0.0f};
    s.set(wall, wb);

    OrbitColliders colliders;
    colliders.sync(s);
    CHECK(colliders.body_count() == 2);
    // The boom from in front of the wall to behind it stops at the wall's near face (z = -5.75).
    OrbitCamera cam;
    cam.target = {0.0f, 1.0f, 0.0f};
    cam.yaw = kPi;  // looking down +Z: the eye goes towards -Z, through the wall
    cam.pitch = 0.0f;
    cam.distance = 10.0f;
    CHECK(cam.resolve(colliders.world()));
    CHECK(cam.effective < 5.75f);
    CHECK(cam.effective > 5.0f);

    Array<Segment3D> segs;
    orbit_wireframe(s, 1.0f, segs, false);
    CHECK(segs.size() == 12 * 2 + 3);  // both boxes and the axes; no sprite quads

    // No camera and no model: nothing to frame through.
    OrbitCamera framed;
    CHECK_FALSE(frame_3d(s, framed));
    // A camera at (0, 2, 9) looking down -Z, pitched 10 degrees down.
    const EntityId camera = s.create_entity("Camera");
    LocalTransform ct;
    ct.position = {0.0f, 2.0f, 9.0f};
    ct.rotation = quat_from_axis_angle({1, 0, 0}, -radians(10.0f));
    s.set(camera, ct);
    s.add_component(camera, refl::type_of<kit::Camera3DComponent>());
    REQUIRE(frame_3d(s, framed));
    CHECK(close(framed.yaw, 0.0f));
    CHECK(close(framed.pitch, radians(10.0f)));
    CHECK(close(framed.distance, 10.0f));
    // The orbit eye lands where the scene's camera stands.
    CHECK(close(framed.eye().x, 0.0f, 1e-2f));
    CHECK(close(framed.eye().y, 2.0f, 1e-2f));
    CHECK(close(framed.eye().z, 9.0f, 1e-2f));
}
