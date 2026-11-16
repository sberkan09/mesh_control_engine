# M-PEDCRASH — pedestrians run from a car crash

Status: steps 1 and 2 done (2026-10-26). Own gameplay milestone (coordinator: pick the most valuable missing GTA-slice system; pedestrians reacting to traffic or crashes was one of the examples).

## Why

Shots already send pedestrians running (M-PEDFLEE 2), but a car that ploughs into a wall at 50 km/h, or into another car, beside a crowd left it strolling on. In the GTA-like slice
a crash is the commonest loud event the player causes.

## What

Step 1. The crowd system (`kit_crowd`, FixedUpdate_Pre) reads `ContactBegin3D` next to `WeaponFired3D`. A contact that closes at `kCrashSpeed` (5 m/s) or more and involves an
entity with `Vehicle3D` is a scare source at its `point`, exactly like a muzzle: pedestrians within `kPedestrianFearRadius` (30 m) run from it for 5 s, whichever side it is on,
and the panic spreads through the existing neighbour hop (M-PEDFLEE 3). One list of sources (shots and crashes, at most 32 a tick) feeds the same loop, so no new field, no saved-format change.
`install_crowd` registers `Vehicle3D` and the `ContactBegin3D` event (both no-ops if the kits already did).

Contacts exist only for bodies with `RigidBody3D::contact_events` (M-CONTACTEVENTS), which `ImpactDamage3D` opts a car into, so a car that can be hurt by crashes also scares the crowd.

Test: `tests/kit/test_crowd.cpp` ("a hard vehicle crash nearby sends pedestrians running, a soft one or a far one does not"): a 9 m/s crash 10 m away sends the pedestrian away from it and it calms
down; a 2 m/s one and a crash 50 m away do not; a hard contact between two non-vehicles is not a crash.

Step 2. A vehicle moving at 6 m/s or more (`kRunDownSpeed`) makes the pedestrians in its path jump clear: within `speed * 2 s + 3 m` ahead of it (along its heading, backwards when reversing) and 2.5 m either side of its line, a pedestrian runs from the vehicle's position for 3 s,
through the same fear list (so the neighbours catch the panic). At most 64 such vehicles per tick; no new field. Test: "a fast vehicle bearing down on a pedestrian sends it running, a slow, a departing or a passing one does not".

## Open items

- The pedestrian runs along its pavement edge away from the vehicle; it does not leave the pavement or cross the road to dodge.
- A wreck that burns or explodes is a separate, louder source (no fire system yet).
