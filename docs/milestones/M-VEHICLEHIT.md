# M-VEHICLEHIT — Cars hurt what they hit (milestone record)

**Status:** done in one step (added 2026-10-23; owner: the gameplay thread; device-free; no Linear issues: the free limit).

**Why.** The GTA feasibility assessment lists "nothing takes damage from a car" as a gap in the slice: pedestrians flee (M-PEDFLEE), police chase (M-OPENWORLD C3), but running someone over did nothing.

**What.** `VehicleRam3D` (28 bytes, saved) and the `kit_vehicle_ram` system (`FixedUpdate_Pre`). The physics layer has no contact events, only triggers, so the hit is a zone in front of the car
(or behind it when reversing) instead of a collision callback. Details in `docs/engine/kit.md` ("Vehicle ram"). Test: `tests/kit/test_vehicle_ram.cpp`
(fast hit, slow / behind / side / far misses, reversing, the once-a-second guard, the driver as instigator and spared).

**Slice test.** `tests/kit/test_gta_slice.cpp` drives the whole loop across modules: a driver runs a civilian over, a police unit witnesses it, the wanted level rises and the unit leaves Idle.

## Open items

- A zone is not a contact: a car sliding sideways past a body can miss, a body at the zone's far edge can be hit a frame early. Tuning needs a street scene.
- The driver is the instigator, so running over a civilian is a `Wanted3D` crime; whether pedestrians hit by a car should also scream (`Fear`) is not wired.
- Hurting the car itself (crash damage) is not part of this step.
