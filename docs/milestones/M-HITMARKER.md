# M-HITMARKER: hit and kill marker (2026-10-31)

Kit: `HitMarker3D` (`mce/kit/hit_marker.h`), system `kit_hit_marker` (FixedUpdate_Pre, after `kit3d_health`). It reads the existing `WeaponHit3D` and `Died3D` events, so the engine core is unchanged (no new event).

- A `WeaponHit3D` whose `owner` holds `HitMarker3D` and whose `target` is another living entity with a `Health3D` sets `timer = duration` (0.15 s) and counts `hits`. Walls, the shooter itself and hits on corpses do nothing.
- A `Died3D` whose `killer` holds it sets `kill_timer = 2 * duration`. A killing shot lights the kill marker only (the target is already dead when the hit is read).
- Both timers count down in `FixedUpdate_Pre` and are `Transient`; `duration` is saved.

Sample (`09_openworld`): `HitMarker3D` on the Player; `hud.mcui` gets a white "x" bound to `HitMarker3D/timer` and a red "X" bound to `HitMarker3D/kill_timer`, both `range [0.001, 10]` so they show only while the timer runs, centred on the crosshair.

Tests: `tests/kit/test_hit_marker.cpp` (3 cases), the MCP openworld test checks the component. Limits: no sound on a hit (a `PlaySfx` hook is a small follow-up), no per-body-part marker, no fade (it blinks on/off).
