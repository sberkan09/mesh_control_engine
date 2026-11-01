# M-IMPACTFX: sparks where a shot lands (2026-10-31)

Kit: `ImpactFx3D` (`mce/kit/impact_fx.h`), system `kit_impact_fx` (FixedUpdate_Post, after `kit_effects`). Put it on an entity with a `ParticleEmitter3D` (rate 0, a short bright look). Every `WeaponHit3D` (any shooter, wall or body) moves the entity to the hit point, lifted `lift` (0.03 m) along the surface normal, and the next tick fires one burst of `count` particles there (the move has to reach the world transform first). One emitter is a pool of one: two hits in one tick show only the last, which is fine for an automatic gun at 6 shots a second.

Sample (`09_openworld`): an "Impact sparks" entity (orange-yellow additive sparks that spray up and fall) with `ImpactFx3D { count: 10 }`. It rests under the map until the first hit.

Tests: `tests/kit/test_impact_fx.cpp` (move and delayed burst, last hit wins). Limits: sparks always spray up (not along the normal), no decal or bullet-hole mark, no per-material look (same sparks on flesh and concrete), no tracer.
