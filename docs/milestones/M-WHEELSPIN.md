# M-WHEELSPIN — Car wheels that turn and steer (milestone record)

**Status:** done (2026-11-17), steps 57a to 57d: 57a (plan, #1280), 57b (kit, #1281), 57c (split models, #1282), 57d (sample, this record's last PR). **Owner: routine 2 thread (the M-VISUAL line).**
Follow-up of M-DRIVEFEEL and M-OPENWORLD-GAPS section E9 ("Still open: wheel spin (the Kenney car is one mesh), door and seat
transitions"). No Linear issues (free limit); this record is the tracker. No step writes a shader or touches `render::`.

**Finding that shapes the plan.** The engine side already exists: `Vehicle3D` names optional entities `wheel_fl`, `wheel_fr`,
`wheel_rl`, `wheel_rr`, and `kit3d_vehicle_state` writes the physics wheel pose (steer angle, spin, suspension) to them every
tick. The gap is content and lifetime: the importer merges the Kenney car into one mesh, so the sample's cars have no wheel
entities, and wheel entities are root entities that would outlive a removed car.

**Not in scope:** a shader, a `render::` field, a physics change, wide headers, door or seat transitions (next milestone),
skid marks, wheel dust.

## Steps

| Step | What | Module | Status |
|---|---|---|---|
| 57a | This plan | docs | done (#1280) |
| 57b | Kit: wheel entities follow the vehicle (placed in the body frame when parented, destroyed with the car); tests in `tests/kit/test_vehicles3d.cpp` (steer angle on the front pair, spin advancing with speed, no leak after the car is removed, scenes without wheel entities bit-identical) | kit | done (#1281) |
| 57c | Generator: deterministic split of the Kenney car glb into a body mesh and one wheel mesh, written as assets | samples/09_openworld | done (#1282) |
| 57d | Sample: every car prefab (player, traffic, police) gets four wheel entities and `Vehicle3D` track, axle and radius matched to the model; `test_mcp_openworld` drive check that a wheel rotates while the car moves; user guide 18-samples; M-OPENWORLD-GAPS and M-OPENWORLD-LOOK updated | samples/09_openworld, tests/mcp | done |

57c needs no kit change and 57b needs no asset, so they can run in parallel; 57d follows both.

## Acceptance

- In a captured frame of a moving car the wheels show a different rotation than a frame earlier, and the front pair is turned
  while steering.
- Removing a car leaves no wheel entity behind.
- Scenes without wheel entities keep their state hashes; existing goldens only change where a car now has wheels.
- `local_ci.sh --task` is green on each PR, and no step adds a test over 60 s.

## Risks

- **Wheel geometry mismatch.** If `Vehicle3D` track and radius differ from the model, wheels float or sink; 57d derives them from
  the split mesh bounds and the drive check compares the wheel centre height with the radius.
- **Entity count.** Four more entities per car multiplies by the traffic count; 57d keeps the traffic count and checks the frame
  budget in the existing performance test.
- **Handling.** Only visuals are added; the physics vehicle is untouched, so handling is bit-identical.

## What was built

- Each of the six Kenney cars is a body model (the SUV keeps its spare wheel) and one wheel model, split by `tools/car_split.py`.
- A car is the physics root, the body as its child, and four hub entities named in `Vehicle3D` (`wheel_fl` .. `wheel_rr`), each with the wheel model as
  its child (the left ones turned half a turn so the outer face points out). Hubs start 1000 m below and the engine places them on the first tick,
  so a spawned car never shows wheels at the origin.
- `Vehicle3D` track, axles, wheel radius and width come from the model, and the body model stands on the road at the car's rest height
  (`CAR_REST_HEIGHT` 0.79 m, `wheel_y` from it), so the tyres touch the road. Before, the model floated about 0.4 m above it.
- The wheelbase of these toy cars is shorter than the old default axles (1.25 m instead of 2.6 m): handling is tighter, `steer_assist` keeps
  it calm at speed. `test_mcp_openworld` drives the car and checks a wheel rotates.
