# M-PARTICLEFLOOR — Keep the camera-centred weather emitters above the ground (milestone record)

**Status:** planned (2026-11-02), steps 32a to 32c. **Owner: routine 2 thread (the M-VISUAL line).** Closes the open follow-up "a box strictly above the ground" recorded in M-BUTTERFLY and M-BAT (and true for M-SNOWSPARKLE). No Linear issues (free issue limit); this record is the tracker.

**Problem.** The butterfly (`0xFFFF0006`), dusk bat (`0xFFFF0007`) and snow sparkle (`0xFFFF0008`) emitters are placed at `scene.view().position` with `box.y = look.height`, a half extent. The spawn volume therefore spans `camera.y - height .. camera.y + height`. With a camera at eye height (about 1.6 m) and heights of 3 m (butterflies), 6 m (bats) or 1.2 m (sparkle), part of the volume lies under the ground and those specks spawn inside the terrain. (The leaf, firefly, pollen, dust and flock emitters already add `+ height` to the position and are not touched.)

**Goal.** A pure helper turns the camera height, the half extent and an assumed eye height above the ground into an emitter centre height whose box floor sits at the assumed ground (`camera.y - eye`) and never lower: `particle_floor_center_y(camera_y, half_height, eye_m) = camera_y - eye_m + half_height` (so the spawn range is `camera.y - eye .. camera.y - eye + 2 * half_height`). The three extracts use it. The eye height is the authoring field `eye_m` (default 1.6, `Range{0, 4}`) added to `DuskBat`, `Butterfly` and `SnowSparkle`, or, for the seam, one shared constant `kDefaultEyeM`.

**Not in scope:** sampling real terrain or physics height under the camera (needs a query), changing the other five emitters, new render fields, shaders, and changing `Season`, `Weather` or the sun.

## Design

- **Shared pure helper (kit, 32a).** `mce/kit/particle_floor.h`: `kDefaultEyeM = 1.6f`, `particle_floor_center_y(f32 camera_y, f32 half_height, f32 eye_m)`; NaN or negative inputs fall back (eye 1.6, half height 0, camera 0); finite and polynomial only. A small header with no component, so no registration, padding, capabilities or golden change. Tests in `tests/kit/test_particle_floor.cpp`.
- **Extract (kit, 32b).** In the butterfly, dusk bat and snow sparkle blocks of `kit3d_extract.cpp`: `em.position = scene.view().position;` becomes the camera position with `y = particle_floor_center_y(cam.y, look.height, kDefaultEyeM)`. Nothing is written or hashed; scenes without these components stay bit-identical. Scenes that do carry them (`12_weather`) change their drawn frame only; the Luau pins are values, not frames. The affected extract tests (`test_butterfly_extract`, `test_dusk_bat_extract`, `test_snow_sparkle_extract`) are updated for the new emitter position.
- **No authoring field in the first cut:** the constant is enough; a per-component `eye_m` is a follow-up if a scene needs a different camera height (it would touch three component layouts, golden `components` unchanged but padding tests and capabilities rows change).
- **Determinism.** The position depends only on the camera and the authoring; no state.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 32a. Pure helper | `mce/kit/particle_floor.h` and `tests/kit/test_particle_floor.cpp` | kit | Sonnet |
| 32b. Apply to the three emitters | the three `em.position` lines, test updates, changelog | kit | Sonnet |
| 32c. Docs and close | the `08-3d` sentence, the M-BUTTERFLY / M-BAT follow-up marked done, milestone status, ROADMAP row | docs | Sonnet |

No step needs Opus: no shader or draw path changes.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 32a | done | 1019 | `mce/kit/particle_floor.h` (`kDefaultEyeM`, `particle_floor_center_y`) and `tests/kit/test_particle_floor.cpp` (6 cases). Nothing calls it yet. |
| 32b | planned | | |
| 32c | planned | | |

## Follow-ups

- A per-component `eye_m`, or the real height under the camera from a physics or terrain query.
