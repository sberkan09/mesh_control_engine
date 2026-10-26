# M-TRAFFICAVOID — traffic goes around a wreck

Status: done 2026-10-26 (steps 1, 2a, 3, 4a, 4b and 4c). Own gameplay milestone (coordinator: "start the lane change, short design note first, then step-by-step PRs").

## Why

M-CARWRECK leaves a dead car standing in its lane. `traffic_drivers` sees it as the car ahead (the occupancy table holds every `TrafficDriver3D`
car, a wreck's `drive = 0` only stops it writing pedals), so everyone behind it queues until `despawn_radius` or the wreck's `destroy_on_death`
timer. On a one-lane road nothing else is possible; on a road with two lanes in the same direction a car should simply take the free one.

## What the code gives us

- `TrafficDriver3D` holds `lane`, `next`, `after`, `s`; `locate()` re-projects the car onto `lane` every tick and walks `lane = next` when it passes the end. A
  car more than `lost_distance` (6 m) off its lane re-snaps. Lane centres on one road are about one lane width (3-4 m) apart, so changing `lane` to a neighbour
  and letting pure pursuit steer is within what `locate` accepts: the steering simply aims at a point on the new lane `look_ahead` metres on.
- `RoadLane` carries `road`, `node` (road lane: the node it runs to), `index` (0 = innermost) and `kind`. Two road lanes with the same `road` and `node` and
  indices that differ by one run the same way side by side. No neighbour table exists yet; it is computed once per network from those fields.
- The occupancy table (`Occupancy::ahead(lane, after, self)`) answers "what is on that lane near s" in O(log n).

## Rules (step 1)

1. **Blocked by a wreck.** The leader in `drive()` is a *blocker* when its `TrafficDriver3D::drive == 0` (a wreck, or a car a game parked) and it is slower than
   0.5 m/s, and the gap is under `dodge_distance` (30 m, scaled by speed: at least the braking distance + 10 m).
2. **A free neighbour.** On the same `road` and `node` as the car's lane, index +-1, kind Road. Free means: no car on it between `s - 10` m behind and `s + gap + 8` m
   ahead of the blocker (checked in the occupancy table by lane `s`; the neighbour's `s` is the projection of the car's position), and the neighbour is not itself
   blocked within that window. Both sides are tried, the lower index first (deterministic).
3. **The change.** `lane` = the neighbour, `next` / `after` = none (chosen again by `choose()` for the new lane), `s` re-projected, `stopped_for` = 0. Pure pursuit does the
   rest. The car stays in the new lane (it does not drift back; the next blocker is dealt with the same way).
4. **Speed.** While passing the blocker the car keeps the IDM against whatever is ahead in the new lane; it does not speed up for the manoeuvre.
5. **Determinism.** No new randomness; the choice depends on the occupancy table and lane indices only, the table is sorted. `TrafficDriver3D` gains no field in step 1
   (no component size change), so saves and state hashes of existing scenes stay as they are.

Not in step 1: oncoming-lane overtakes on single-lane roads (needs the oncoming flow to be checked), going around a blocker inside a junction connector, signalling.

## Steps

1. **Neighbour table + dodge in `traffic.cpp`**, tests in `tests/kit/test_traffic.cpp` (a two-lane road with a wreck: the follower ends up in the other lane and passes it; a one-lane
   road: it queues as before; the other lane occupied: it waits; two runs equal).
2. **Polish:** return to the right-hand lane after passing, a `dodge` toggle on `TrafficDriver3D` (a new field, so a saved-format change in its own PR), the same rule for any
   parked car, an oncoming-lane overtake on empty single-lane roads.

## Open items

- Cars already inside a connector (a junction) do not dodge; a wreck in a junction blocks it until it is removed. Tow-away through `Health3D::destroy_on_death` is the answer there.
- The follower's pedals are written by the same IDM; a very short gap at the moment the wreck appears can still end in a touch. Acceptable: `ImpactDamage3D` makes that a crash like any other.

**Step 1 (done 2026-10-25).** `Drive::dodge` and `Drive::neighbour` in `traffic.cpp`: a car whose leader is a stopped (`< 0.5 m/s`) car with `drive = 0` within `max(30 m, braking distance + 10 m)` switches `lane` to a free same-direction neighbour (lower index first), `next` / `after` cleared. No new field. Tests in `tests/kit/test_traffic.cpp` ("traffic avoid": two lanes go around, one lane queues, the other lane blocked waits). Known limit (closed by step 2): two cars dodging in the same tick read the same occupancy table.

**Step 2a (done 2026-10-25).** `Drive::claimed`: a lane a car changed into this tick is recorded with its `s`, and a later car in the same tick treats that window as occupied, so a platoon queued behind a wreck goes around one after the other instead of side by side. Test: "traffic avoid: two cars queued behind a wreck both get around it". Still open: oncoming-lane overtake on one-lane roads.

**Step 3 (done 2026-10-25).** `TrafficDriver3D::home` (saved, engine-written): the lane a car left to go round a wreck. `Drive::rejoin` puts it back there once nobody is within 10 m behind / 12 m ahead of it in that lane, no other undriven stopped car lies within 60 m ahead there (it would only dodge again), and no other car claimed that gap this tick; it forgets `home` if it drives on to another road piece. Test: "traffic avoid: once past the wreck the car returns to the lane it left"; the earlier dodge tests now check that the car was in another lane on the way.

## Step 4 design: oncoming-lane overtake on one-lane roads (not built yet)

A road with `lanes: 1` has no same-direction neighbour, so a car behind a wreck queues. The only free asphalt is the oncoming lane (the road lane of the same `road` with the opposite `node`, `index` 0).

- **Model.** The car keeps its own `lane` and `s` (all lane following, queueing and junction logic stay as they are) and gets a saved lateral offset `TrafficDriver3D::offset` (m, 0 = centred in its lane). Pure pursuit aims at the lane point shifted sideways by `offset`; nothing else reads it. A new saved field, so a saved-format and state-hash change: its own seam PR first (the header is included by 10 files).
- **When.** Its leader is a stopped car with `drive = 0` (as in step 1), the oncoming lane has no car within `v * t + 60 m` of the passing window (both directions are read from the occupancy table: oncoming `s` runs the other way, so the window is `L - s`), and the wreck is not in or near a junction connector.
- **While passing.** `offset` moves to the oncoming lane's centre at a bounded rate; the car ignores the wreck as a leader only while its lateral distance is above a car width. If an oncoming car turns up inside the safe distance, it brakes and moves back (or stops) rather than finishing the pass.
- **Return.** Once its rear is past the wreck by a car length, `offset` goes back to 0 at the same rate.
- **Tests.** One-lane road with a wreck and an empty oncoming lane: the car passes it. Oncoming car arriving: the pass is not started or is abandoned and the two never overlap. Two runs equal.

Steps: 4a the `offset` field and pursuit (seam), 4b the decision and its tests, 4c polish (several cars in a row, the oncoming flow side).

**Step 4a (done 2026-10-25).** `TrafficDriver3D::offset` (saved, settable, `Range{-8, 8}`): metres to the right of the lane centre line that pure pursuit aims for; the car's `lane` and `s` are unchanged. Nothing sets it yet (4b does). Test: "traffic: offset shifts where a car drives inside its lane".

**Step 4b (done 2026-10-26).** `Drive::overtake`: on a road with no same-direction neighbour lane, a car whose lane holds a standing undriven car ahead (same trigger as step 1, but 45 m at least) moves `offset` to the left, to the oncoming lane's centre (further if the wreck has drifted: it aims 3.2 m left of the wreck's measured side), at 2.5 m/s, when the oncoming lane has no car within 100 m ahead of the car's position (read from the occupancy table, `s` of the oncoming lane runs the other way). Once `offset` puts the car 3 m to the left of the wreck, the wreck is skipped as its leader and the IDM lets it accelerate past. Level with the wreck (within 8 m ahead of it) it carries on whatever comes; if an oncoming car appears while it is still further back, `offset` returns to 0 and it queues. Once the wreck is 10 m behind, `offset` returns to 0. A rightward (positive) offset is left alone. Tests: \"traffic avoid: on a one-lane road a car passes the wreck through an empty oncoming lane and comes back\" and \"... queues behind the wreck while the oncoming lane is blocked\". Known limits: an oncoming car that appears after the car is level with the wreck is not avoided (the IDM still brakes for it as a leader only in its own lane); the pass does not start inside a junction connector; several cars in a row (4c).

**Step 4c (done 2026-10-26) and close.** Test \"traffic avoid: several cars in a row each pass the wreck through the oncoming lane\": three cars 22 m apart on a one-lane road each go round the wreck and come back (never closer than 2.8 m to the wreck or 4.5 m to each other). No code change was needed: the cars ahead are ordinary leaders, and the wreck is skipped only by the car that is out to its left. Not built, and out of scope for this milestone: an oncoming car that appears after a car is level with the wreck is not yielded to by either side (the oncoming car's leader search reads its own lane only); cars do not dodge inside junction connectors; pedestrians and parked-car doors are not part of it. They are listed as follow-ups for a later traffic milestone.
