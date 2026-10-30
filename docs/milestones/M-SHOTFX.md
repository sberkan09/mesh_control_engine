# M-SHOTFX: muzzle flash and gun kick (2026-10-30)

Closes the muzzle-flash part of "no visible feedback on shooting" in `M-OPENWORLD-GAPS.md` (section B).

- Kit (`effects.h/.cpp`): `FireOn::Fired` (appended, value 8). A `WeaponFired3D` names its weapon entity and the `Weapon3D.aim_from`
  entity (the camera a player's gun aims from), so a `ParticleBurst` / `SfxTrigger` / `ShakeTrigger` with `on Fired` fires on either.
- Sample: the 09_openworld Camera carries a short additive `ParticleEmitter3D` at the muzzle (6 m ahead, where shots leave),
  `ParticleBurst {6, Fired}` and `ShakeTrigger {0.04, Fired}`.
- Test: `tests/kit/test_effects.cpp` ("FireOn Fired ...").

Limits: no tracer line, impact sparks or decal, hit marker or shell casing; officers' and other NPC shots show no flash (they have no
trigger); the flash is a fixed offset from the camera, not from a gun model.
