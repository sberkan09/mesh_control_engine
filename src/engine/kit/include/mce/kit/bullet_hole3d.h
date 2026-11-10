#pragma once
// Bullet holes where shots land (M-SHOTMARKS). kit::install_stock installs it.
//
//   BulletHole3D  on an entity that also has a LocalTransform and a Decal3D: one slot of a ring of holes. Author
//                 as many as you want (24 is a good number) with the same Decal3D; they all start hidden.
//
// Every WeaponHit3D whose `target` has no Health3D (a wall, a car, the ground) takes the next slot: the hole with the
// lowest `serial` (a never-used one first, then the oldest), moves it to the hit point, turns its +Y along the surface
// normal (a decal projects down its local -Y, onto the surface), sizes its Decal3D to (`size`, `depth`, `size`), shows
// it and stamps `serial` with the highest serial plus one. After `life` seconds it fades out over `fade` seconds and
// hides again. Two hits in one tick take two slots. Hits on characters leave nothing.
//
// System: "kit_bullet_holes" (FixedUpdate_Post). Needs install_combat3d first (it registers WeaponHit3D), as
// install_stock does.

#include "mce/math/math.h"
#include "mce/refl/macros.h"

namespace mce {
class World;
}

namespace mce::kit {

struct BulletHole3D {
    f32 size = 0.25f;  // metres, the decal's x and z
    f32 depth = 0.3f;  // metres, the projection depth (y): how far into curved surfaces it reaches
    f32 life = 20.0f;  // s a hole stays fully visible
    f32 fade = 4.0f;   // s it then takes to disappear
    f32 alpha = 1.0f;  // opacity at full visibility (the Decal3D's colour alpha is driven from it)
    f32 age = -1.0f;   // engine-written: s since it was placed; < 0 = unused (hidden)
    u32 serial = 0;    // engine-written: placement order, 0 = never used
};
MCE_COMPONENT(BulletHole3D, (size, Range{0.01f, 5}), (depth, Range{0.01f, 5}, Help{"projection depth in metres"}),
              (life, Range{0, 3600}, Help{"s fully visible"}), (fade, Range{0, 600}, Help{"s to fade out afterwards"}),
              (alpha, Range{0, 1}), (age, ReadOnly), (serial, ReadOnly))

// Registers the component and adds the system. Idempotent.
void install_bullet_holes(World& world);

}  // namespace mce::kit
