# M-SEATMOVE — Getting into and out of cars with a glide (milestone record)

**Status:** done (2026-11-17), steps 58a (kit, #1289) and 58b (sample, this record's last PR). **Owner: routine 2 thread (the M-VISUAL line).**
Follow-up of M-OPENWORLD-GAPS section E9 ("door and seat transitions"). No Linear issues (free limit); this record is the tracker.
No step writes a shader or touches `render::`.

**Why this one.** Boarding and leaving a car teleport the rider: one frame it stands beside the car, the next it sits in it. The
model has no door to swing (only the delivery van has a door mesh), so the visible fix is a short glide of the rider itself.

**Not in scope:** a door mesh that swings, a sit-down animation clip, the camera path (it already follows the car or the rider
on the same tick), any change to who can board or when.

## Design

- `VehicleSeat3D::transition` (seconds, 0 = instant, the default, so every existing scene behaves as before).
- `Rider3D` gains engine-written `phase` (0 none, 1 boarding, 2 leaving), `phase_time`, `from_pos`, `to_pos`, `from_rot`, `to_rot`.
- Boarding: as before (controller taken off, input drives the car) plus phase 1; `kit3d_seat_pose` eases the rider from where it
  stood to the live seat pose (smoothstep, slerp).
- Leaving: the exit point is chosen when Interact is pressed; phase 2 eases from the seat to it, the seat stays taken and
  Interact is ignored, then the usual step-out runs (controller back, seat freed, handbrake).
- Death, a destroyed car or a removed seat end a glide through the existing cleanup (`step_out` resets the phase).

## Steps

| Step | What | Module | Status |
|---|---|---|---|
| 58a | The kit: `transition`, the phases and the glide; `tests/kit/test_seats3d.cpp` (halfway pose, Interact ignored mid-glide, seat taken while leaving, controller back after, 0 = instant is the existing cases); user guide chapter 9 | kit | done (#1289) |
| 58b | `09_openworld`: `transition` 0.35 on the player's and parked cars; `test_mcp_openworld` checks a rider is gliding (phase 1) a few ticks after boarding and seated and settled after; M-OPENWORLD-GAPS and M-OPENWORLD-LOOK | samples/09_openworld, tests/mcp | done |

## Acceptance

- Scenes with `transition` 0 keep their behaviour (the existing seat tests pass untouched).
- A boarding rider is between its start and the seat part-way through, and at the seat when the glide ends; leaving ends at the exit point.
- `local_ci.sh --task` is green on each PR, and no step adds a test over 60 s.

## Risks

- **Moving car while leaving.** The exit point is fixed in the world at the press and the car must be under `max_exit_speed`, so
  the glide covers at most about a metre of drift; the exit sphere test is not repeated at the end.
- **State hash.** `Rider3D` grew (80 bytes) so state hashes of scenes with a rider change once; nothing records them.
