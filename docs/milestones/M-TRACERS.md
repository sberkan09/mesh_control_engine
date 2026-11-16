# M-TRACERS: tracer lines for shots

**Status:** done (2026-11-10; PRs #1231 plan, #1232 seam, #1233 sample). Closes the last open limit of [M-SHOTFX](M-SHOTFX.md) ("no tracer line") after
[M-SHOTMARKS](M-SHOTMARKS.md) added the bullet holes. Game layer only: one kit module and the `samples/09_openworld` scene; no ECS,
render, physics or audio core. Needs no device or account. It only places existing `MeshRef` entities, so it does not touch the
M-VISUAL lighting and reflection work.

## Why

A shot is a flash and a spark; the line between the muzzle and the hit is invisible, so a long shot down the avenue reads as nothing
and an officer's shot at the player cannot be seen coming. `WeaponFired3D` already carries `origin` and `end`.

## Design

`Tracer3D` on each slot of a ring of authored entities that also carry a `LocalTransform` and a `MeshRef` (a thin unit box with an
emissive material; the scale is set per shot). System `kit_tracers` (FixedUpdate_Post) reads `WeaponFired3D`, takes the slot with the
lowest `serial` (a never-used one first, then the oldest, the same rule as `BulletHole3D`), places it at the middle of `origin` to `end`,
turns its length axis along the shot, scales it to (`width`, `width`, the length), shows it and stamps `serial`. After `life` seconds
(short, 0.06 s by default) it hides. No stored cursor, no entity references. A shot shorter than `min_length` draws nothing, so a point-blank
shot does not draw a stub. All shooters draw one (the player and officers); a per-shooter filter is a later field if wanted.
Padding-free state with `static_assert(sizeof)`.

## Steps (seam, feature, polish; each its own PR)

| Step | What | Status |
|---|---|---|
| 1 seam | `Tracer3D`, system `kit_tracers`, unit tests (a shot shows the next slot at the middle, turned and scaled along the shot, the ring reuses the oldest, two shots in a tick take two slots, a short shot draws nothing, the line hides after `life`, no slots is harmless). No sample change. | done (2026-11-10, #1232) |
| 2 feature | Sample: a ring of 16 thin boxes with an emissive material written by `gen_assets.py` (the mesh and material are generated, no third-party asset), `test_mcp_openworld` inspect check, regenerated assets, the temporary `sample_coverage` line removed. | done (2026-11-10, #1233) |
| 3 polish | User-guide section (with step 1), closing doc, the "no tracer line" notes struck through in M-SHOTFX and M-OPENWORLD-GAPS. | done (2026-11-10) |

## Decisions taken (change them in the plan PR if wrong)

- Boxes, not a line primitive: a stretched emissive box needs nothing from the render side and shows up in bloom through `emissive_tint`.
- One short life for all tracers; a fade is not worth a second material parameter at 0.06 s.
- The look (width, brightness) is a first guess judged from data, not from a real display.

## Notes

- Step 1 adds a component: the golden component count, `capabilities.md` and the three `engine_components.cpp` files move with it.
- Linear: the Backlog holds only work that needs real devices or accounts, so this file is the record.
