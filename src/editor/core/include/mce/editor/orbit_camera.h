#pragma once
// The editor's 3D orbit camera and its collision gate (ROADMAP M5b, ADR-0029 §3).
//
// OrbitCamera is pure math (headless, tested in CI): yaw/pitch/distance around a target, with
// orbit, pan and dolly driven by viewport mouse deltas. resolve() is the camera-collision gate: a
// sphere swept along the boom from the target to the eye with World3D::camera_sweep (static and
// kinematic geometry only), so the camera stops in front of a wall or above the floor instead of
// looking through it, and slides back out as the obstacle clears.
//
// OrbitColliders builds that world from the scene document: every entity with a LocalTransform and
// a Collider2D becomes a static box (the 2D shape extruded `depth` metres along Z, centred on the
// sprite plane), which is what a 2.5D look at a 2D level collides with, and every kit3d
// BoxCollider3D the box the game builds from it.

#include "mce/editor/editor_scene.h"
#include "mce/math/math.h"
#include "mce/physics3d/navmesh3d.h"
#include "mce/physics3d/world3d.h"
#include "mce/render/scene3d.h"

namespace mce::editor {

struct OrbitCamera {
    math::vec3 target;
    f32 yaw = 0.6f;     // radians about +Y; 0 looks down -Z
    f32 pitch = 0.45f;  // radians below the horizon (positive looks down)
    f32 distance = 12.0f;
    f32 fov_y = math::radians(45.0f);
    f32 min_distance = 0.5f;
    f32 max_distance = 500.0f;
    f32 probe_radius = 0.2f;  // the swept sphere: the camera keeps this far from surfaces

    // Distance actually used this frame: `distance` pulled in by the last resolve().
    f32 effective = 12.0f;

    // Mouse drag: `delta_px` viewport pixels. Pitch stays within ±89°.
    void orbit(math::vec2 delta_px, f32 radians_per_px = 0.006f);
    // Drags the target in the view plane so the point under the cursor follows it.
    void pan(math::vec2 delta_px, rhi::Extent2D viewport);
    // Wheel notches: each one moves 15% closer (positive) or further.
    void dolly(f32 notches);
    // Frames a box: target at its centre, distance so it fits the vertical field of view.
    void frame(const math::aabb3& box);

    // Unit vector from the target towards the eye.
    math::vec3 back() const;
    math::vec3 eye() const { return target + back() * effective; }
    // The camera at the effective distance.
    render::Camera3D camera() const;

    // The collision gate: sets `effective` to the boom length that keeps the probe sphere clear
    // of `world` (== distance when nothing is in the way). Returns true when the boom was cut.
    bool resolve(const physics::World3D& world);
    // No collision world: the eye sits at `distance`.
    void unresolved() { effective = distance; }
};

class OrbitColliders {
public:
    static constexpr f32 kDefaultDepth = 1.0f;

    // Rebuilds the world from the document when its revision moved (cheap otherwise).
    void sync(const EditorScene& scene, f32 depth = kDefaultDepth);
    const physics::World3D& world() const { return world_; }
    u32 body_count() const { return bodies_; }
    // Bounds of every collider box (empty: an inverted box).
    const math::aabb3& bounds() const { return bounds_; }

private:
    physics::World3D world_;
    u64 revision_ = ~u64(0);
    f32 depth_ = kDefaultDepth;
    u32 bodies_ = 0;
    math::aabb3 bounds_{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
};

// Points `orbit` at a kit3d scene: through its primary Camera3DComponent (the pivot ten metres
// ahead), else around its MeshRef origins. False when the scene has neither.
bool frame_3d(const EditorScene& scene, OrbitCamera& orbit);

// 3D wireframe for the orbit view: the collider boxes (2D extruded and kit3d), every sprite quad
// (unless `sprite_quads` is false: a kit3d scene draws its own models) and a cross at each
// selected model, as segments.
struct Segment3D {
    math::vec3 a, b;
    math::Color color = math::Color::white();
};
void orbit_wireframe(const EditorScene& scene, f32 depth, Array<Segment3D>& out, bool sprite_quads = true);

// The navmesh the scene's first NavMeshSurface3D points at (its `baked` .mcnav, M-GAMEPLAY3D 1),
// for the orbit view: the walkable floor's triangle edges (tinted by area) and the off-mesh links.
// sync() reloads when the bake changes (another GUID, or a re-bake under the same one).
class NavMeshPreview {
public:
    static constexpr usize kMaxSegments = 60000;

    void sync(const EditorScene& scene, asset::AssetSystem* assets);
    // Nothing when there is no bake or the surface's `show` is off.
    void segments(Array<Segment3D>& out) const;
    const physics::NavMesh3D& mesh() const { return mesh_; }

private:
    physics::NavMesh3D mesh_;
    asset::Guid guid_;
    u64 hash_ = 0;
    bool show_ = false;
    Array<math::vec3> tris_;
    Array<u8> areas_;
    Array<math::vec3> links_;
};

// Projects `seg` through `view` to viewport pixels (origin top-left, +y down), clipped against
// the near plane. False when the whole segment is behind the camera.
bool project_segment(const render::View3D& view, const Segment3D& seg, math::vec2& a_px, math::vec2& b_px);

}  // namespace mce::editor
