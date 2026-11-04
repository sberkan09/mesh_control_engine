# M-LAMPGLOW — Lamp glow: the bulb or flame mesh dims with its flickering light, render-side only, no shader (milestone record)

**Status:** done (2026-11-03), steps 37a to 37c (PRs 1066, 1067, 1068). Steps 37a and 37b shipped together in one PR (one small kit change). **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-FLICKER (the light now wavers, but the lamp mesh that carries it stays at full emissive, so a failing bulb's pool of light dims while the bulb itself shines on) and of the 3D visual assessment (lights and emissives should read as one object). No Linear issues (free issue limit, Linear unreachable); this record is the tracker.

**Naming:** no type, file or milestone called `LampGlow` exists (`projects/`, `src/`, `docs/`, `tests/` checked 2026-11-03). The work extends `LightFlicker3D` with one field instead of adding a component.

**Goal.** `LightFlicker3D` gets an `emissive_follow` field (0..1, default 0): how far the entity's own `MeshRef.emissive_tint` follows the flicker. `Kit3DExtract` multiplies the instance's `emissive_tint` by `lerp(1, light_flicker_scale(...), emissive_follow)` for an entity that has both a `MeshRef` and an enabled `LightFlicker3D`, at the same render clock and entity salt as its light, so the bulb and its light dim together. **Render-side only: nothing is written to a component and nothing is hashed.** With `emissive_follow` 0 (the default) every existing scene and the extract output are bit-identical.

**Not in scope:** a shader or a new `render::` field, driving other entities' meshes (a lamp whose mesh is a child of the light entity is a follow-up: it needs a parent lookup), colour shifts, per-material-slot control, and anything gameplay can read.

## Design

- **One field, no system, no state.** `emissive_follow` goes into the 4-byte gap before `enabled` (the struct grows from 28 to 32 bytes: `u8 enabled`, `u8 pad_[3]` stay last); `Save`, `Range{0, 1}`, default 0. Old saves and scenes without the key load with 0.
- **Pure helper (37a).** `light_flicker_emissive_scale(const LightFlicker3D&, f64 time_s, u32 entity_salt) -> f32` = `1 + emissive_follow * (light_flicker_scale(...) - 1)`; exactly 1.0f for `enabled` 0, `emissive_follow` 0 or `depth` 0; finite, in `[1 - depth, 1]`.
- **Extract (37b).** In the `MeshRef` instance loop of `kit3d_extract.cpp`, after `inst.emissive_tint = m.emissive_tint;`: if the entity has an enabled `LightFlicker3D` with `emissive_follow > 0`, multiply `inst.emissive_tint` by the helper (the render clock is the same expression the light loops use; compute it once).
- **Sample (37b).** `kit3d_tour`'s "Campfire light" gets a small emissive `MeshRef` if the sample has a suitable ready-made mesh asset; otherwise the sample stays as is and the test in `tests/kit` is the proof.
- **Determinism.** The scale depends on `(seed, entity, frame_time)` only; it never feeds the simulation or the state hash.

## Steps

| Step | What | Modules | Model |
|---|---|---|---|
| 37a. Field and helper | `emissive_follow`, `light_flicker_emissive_scale`, `test_padding` size update, `tests/kit/test_light_flicker.cpp` cases, capabilities regen if the row changes, guide sentence | kit | Sonnet |
| 37b. Extract feed and sample | the mesh instance's `emissive_tint` scale; `tests/kit/test_light_flicker_extract.cpp` cases; the tour entity if a mesh fits | kit, samples | Sonnet |
| 37c. Docs and close | the `08-3d` sentence, milestone status, ROADMAP row, changelog | docs | Sonnet |

No step needs Opus: nothing writes a shader; the emissive tint is a number the renderer already takes.

## Step status

| Step | Status | PR | Notes |
|---|---|---|---|
| 37a | done | 1067 | `emissive_follow` (0..1, default 0, before `enabled`; the struct is 32 bytes) and `light_flicker_emissive_scale` in `mce/kit/light_flicker.h`; helper cases in `tests/kit/test_light_flicker.cpp`. |
| 37b | done | 1067 | The `MeshRef` instance loop multiplies `emissive_tint` by the helper at the render clock (now computed once before that loop and shared with the light loops); `tests/kit/test_light_flicker_extract.cpp` (`LampRig`). The tour sample was not changed: no ready-made lamp mesh fits, the tests are the proof. A `StaticMerge3D`-merged mesh does not flicker (merged meshes skip the instance loop). |
| 37c | done | 1068 | The `08-3d` sentence, milestone closed, ROADMAP row done. |

## Follow-ups

- A lamp mesh on a child entity of the light entity (parent lookup).
- A tint shift towards orange at the low end of the fire style.
- A real GPU/windowed look at the bloom the emissive scale feeds.
