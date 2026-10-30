# M-CARWRECK — a destroyed car is a wreck

Status: step 1 done (2026-10-24). Own milestone, defined by the gameplay thread after M-CONTACTEVENTS (the feasibility assessment lists traffic and
car combat as the open "game kit" layer).

## Why

`ImpactDamage3D` and `VehicleRam3D` can now kill a car (`Health3D::dead`), but nothing reacted: a dead traffic car kept its `TrafficDriver3D` throttle, a dead
player car kept answering input. A wreck must stop being driven.

## What

1. **`kit_wreck`** (FixedUpdate_Pre, in `impact_damage.cpp`, installed with `install_impact_damage`; no new component, so no saved-format or golden churn):
   for every dead `Health3D` entity that has a `Vehicle3D`: `use_input = 0`, throttle / steer / brake 0, handbrake 1, and `TrafficDriver3D::drive` /
   `VehicleDriver3D::drive` = 0. Removal is the existing `Health3D::destroy_on_death` + `linger`, or the wreck stays as scenery. Test:
   `tests/kit/test_impact_damage.cpp` ("wreck: ...").

## Open items

- A wreck stays in its traffic lane and the cars behind it queue (the leader logic sees a car ahead); a spawner removes it beyond `despawn_radius`. Clearing it is
  already a prefab setting: `Health3D::destroy_on_death` with a `linger` removes the dead car and the spawner fills the slot. Going around it (a lane change) is a
  follow-up and a design task in `traffic.cpp`.
- Fire, smoke and an explosion on death (effects from `Died3D`) are content, not code: a `FireOn::...` emitter on the prefab; no engine change planned.
- The driver is not thrown out when the car dies (the seat is freed by `kit3d_seats` when the car is destroyed); stepping out of a wreck needs `max_exit_speed`, which a stopped wreck meets.

Slice test: `tests/kit/test_gta_slice.cpp` (a car driven into a wall: wrecked, driver hurt, input off).

## Step 2: in `samples/09_openworld` (2026-10-30)

Every car (traffic and parked) has `Health3D` and `ImpactDamage3D` (min 6 m/s, 5 hp per m/s, the driver takes 40%); traffic cars have 150 hp and are removed 10 s after dying (the spawner refills), the player's cars 200 hp and stay as wrecks. Parked cars also have `VehicleRam3D` (5 m/s, 12 hp per m/s), so running a pedestrian over hurts and counts as a crime. Traffic cars have no ram zone on purpose (a following car would wreck the one ahead). Still open: fire and smoke on a wreck, police cars have no impact damage, no repair shop.
