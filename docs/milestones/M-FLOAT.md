# M-FLOAT — Objects that ride the waves: an opt-in `Floater` that follows the drawn water surface (milestone record)

**Status:** planned (started 2026-11-12), steps 55a to 55d. 55a is this plan. **Owner: routine 2 thread (the M-VISUAL
line).** Follow-up of M-WATER (`Water`, `water_height_at`, `water_normal_at`, the Luau `water.*` queries), M-WATER2
(`WaterFlow`) and M-FREEZE ("gameplay still sees waves"). No Linear issues (free issue limit); this record is the tracker.
No step writes a shader, so no step needs Opus.

**Naming:** `Floater` and `M-FLOAT` were checked 2026-11-12 with a case-insensitive grep over `src/` (headers): no type
of that name exists. The user guide (9.19m) says "the engine applies no buoyancy force, the script moves the object";
this milestone keeps that true: it adds no physics force, only a pose that follows the drawn surface.

**Goal.** Today a crate or boat that should bob on the sea needs a Luau script that calls `water.height_at` and
`water.normal_at` every tick. M-FLOAT makes that a component: a `Floater` entity is placed on the wave sum the renderer
draws, rises and falls with it, and leans with the surface normal, with no script. Opt-in and deterministic; a scene
without it is bit-identical.

**Not in scope:** buoyancy forces, rigid-body floating or drag (the pose is written, the physics body is not pushed),
displaced-volume or multi-point hull sampling, splash or wake effects, anything under the surface (`WaterIce` does not
stop a float: gameplay still sees waves, M-FREEZE), a shader, a `render::` field or any wide header.

## Design

- **Component.** `kit::Floater {offset_y, tilt, follow, enabled, pad_}` in `mce/kit/floater.h` (a new header, included
  only by the system, the install and the script binding), registered by `install_weather` next to `Water`. Defaults:
  `offset_y` 0 (metres of the pivot above the surface), `tilt` 1 (0 keeps the entity upright, 1 follows the normal),
  `follow` 1 (0..1 blend of the pose toward the surface), `enabled` true. Clamps: NaN to the default, `tilt` and
  `follow` to 0..1. Explicit zeroed padding and a `test_padding` entry, as every component.
- **Pure helper.** `floater_pose(const Water&, const WaterFlow*, const Floater&, vec3 position, f64 scene_time, f32
  wind_dir_deg) -> FloaterPose {y, up}` uses the existing `water_height_at` and `water_normal_at` (flow variants when a
  `WaterFlow` exists). Pure, no state: the same inputs give the same bits.
- **System.** `kit_floater` writes only the `Transform` of an entity that has a `Floater`: the world y and the rotation
  that tips the entity's up axis toward the surface normal, keeping its heading. The scene time lives outside the World,
  so 55b first checks how a system reads it (tick times fixed dt, as the Luau binding does); if it cannot, the system
  reads the same `tick * dt` and the plan records it. A parent-less entity only; a parented floater is left alone and
  logged once (no hidden world-matrix maths).
- **No Water, no motion.** A scene without a `Water`, or a `Floater` with `enabled` 0, is not touched.
- **Luau (55c).** `floater.set{...}` and getters, typed in `definitions.cpp`, bad input raises a named error and writes
  nothing; the sample `13_water` gets a bobbing crate and a boat-like prop.

## Steps

| Step | What | Modules |
|---|---|---|
| 55a. Plan | this record, the ROADMAP row, a changelog fragment | docs |
| 55b. Component, helper, system | `Floater`, `floater_pose`, `kit_floater`, registration, `test_padding`, tests (pose follows `water_height_at`, tilt 0 stays upright, flow variant, no Water untouched, parented entity untouched, determinism hash) | kit, tests |
| 55c. Luau and sample | `floater.set` and getters, `13_water` carries two floaters, the sample hash and golden updated deliberately | script, samples |
| 55d. Docs and close | guide 9.19m completed, capabilities regenerated, row set to done | docs |

## Gates
- A scene without `Floater` has an unchanged state hash and frames; existing sample hashes stay unchanged until 55c.
- `Water`, `WaterFlow` and `WaterIce` are never written by floater code.
- All pose maths is a pure function of authoring, position and scene time; no `rand`, no wall clock.
- No wide header is edited (`scripts/ci/include_fanout.py` on the touched headers).
