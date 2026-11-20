# M-CAMNAMEFUZZ — fuzzing the orbit camera and Make names unique (milestone record)

**Status:** done 2026-11-20 (one step, no defect found). **Owner: the "Geliştirme rutini 3" thread.** Follow-up of M-TABSFUZZ, defined on
the coordinator's instruction to keep opening editor and tooling work. No Linear issues. Tests only.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Fuzz 200 seeds each. Orbit camera: random orbit, pan (zero-sized viewports included), dolly and frame, with occasional wild gestures: target, yaw and eye stay finite, pitch stays within 89 degrees, distance stays within its limits. Make names unique: names from a pool of awkward suffixes and 60 to 63 byte names, random selection: every selected entity ends with a name no other entity has | done 2026-11-20: `test_orbit_camera.cpp`, `test_unique_names.cpp`; no defect. Scene migration (`scene_document`) and the importers were already fuzzed in `tests/fuzz`, so they were not repeated |
