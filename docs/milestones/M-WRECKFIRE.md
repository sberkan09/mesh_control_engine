# M-WRECKFIRE: fire and smoke on a wrecked car (2026-10-30)

Design: section H of `M-OPENWORLD-GAPS.md`.

## Kit (`effects.h`, `effects.cpp`)

- `FireOn::Died` (appended, value 7): fires when a `Died3D` names the entity (the Health3D death path, `apply_damage`).
- `ParticleBurst` also drives a `ParticleEmitter3D` on the entity (`burst = count`, `++burst_version`); `keep_emitting = 1`
  also sets `emitting = 1`, so a wreck keeps smoking and a loaded save still does. The struct stays 12 bytes.
- `SfxTrigger` on an entity with `AudioSpatial` plays 3D at the entity (`attenuation_id`, `position`).

## Sample (`samples/09_openworld`)

`wreck_effects()` in `gen_assets.py`: every car (traffic, parked, police) has an emitter (off), `ParticleBurst` (60, Died, keep_emitting),
`SfxTrigger` (crashes) and `ShakeTrigger` (0.35, Died); the Camera has a `CameraShake3D`.

## Tests

`tests/kit/test_effects.cpp` ("FireOn Died ...", "the Died wreck run twice ..."); `test_mcp_openworld` counts `ParticleBurst` >= 3.

## Limits

One emitter per entity: fire and smoke share a colour ramp. The shake has no distance falloff. No blast damage to neighbours.
Nothing puts the fire out; a repair must set `emitting 0`. A traffic wreck's smoke ends with the car 10 s after death.
No end-to-end MCP kill test (`Health3D.current = 0` does not kill; only `apply_damage` does).
