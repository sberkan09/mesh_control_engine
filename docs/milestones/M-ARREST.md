# M-ARREST — police arrest an offender who stands still

Status: steps 1 to 5 done (2026-10-27). Own gameplay milestone (coordinator: pick the most valuable missing GTA-slice system; wanted level / police was one of the examples). M-OPENWORLD C3 listed arrests as an open item.

## Why

Police could chase and shoot, never catch: a wanted player had only two outcomes, escape or die. In the GTA-like slice a low-level crime that ends with the player giving up is the normal "busted".

## What

Step 1. `Police3D` gains `arrest_range` (2.5 m), `arrest_time` (2 s), `arrest_speed` (1.5 m/s), `arrest_max_level` (1) and the engine-written `arrest_progress`. In `kit3d_wanted`, a unit that sees the offender within range, at a level of
`arrest_max_level` or below, with the offender moving no faster than `arrest_speed` (measured from its position last tick, kept in the system's state), builds progress; it falls back at the same rate otherwise. At `arrest_time` the unit
pushes `Arrested3D { offender, by, position }` and calls `clear_wanted`; the other units go Idle on the next tick. What the arrest costs the player is the game's, on that event. Levels above `arrest_max_level` keep the old behaviour
(chase, shoot). `Police3D` goes from 48 to 64 bytes (a saved-format change by appended fields; old scenes load with the defaults).

Test: `tests/kit/test_wanted3d.cpp` ("a unit arrests an offender who stands still within reach at a low level"): arrested after 2 s of standing still; no arrest when the offender keeps moving, is 5 m away, is at level 2, or the unit has `arrest_range = 0`.

Step 2 (design note: reinforcements without new components). Spawning units from nowhere needs a prefab and a station
concept, which is the game's content; the engine already has the level gate (`Police3D::min_level`). What was missing is
that a unit only reacted to an offender it *saw*. `Police3D::dispatch_range` (m, default 0 = off) closes that: an idle unit
whose `min_level` the offender has reached, and which is within `dispatch_range` of `Wanted3D::last_known` (and not already
at it), takes the offender as its target and goes to Search there (a radio call). Reinforcements are then units placed
around the map with a higher `min_level` and a long range: they stay idle at level 1 and arrive as the level rises. The
search ends after `search_time` as usual. No new component, no spawn; only a field (`Police3D` 64 to 68 bytes, appended).
Test: "an idle unit within dispatch range is sent to the last known place": sent when in range, not when out of range, not
when `min_level` is above the level, not when `dispatch_range` is 0.

Step 3 (police cars, no new component). A police car is a `Police3D` on an entity that also has a `Vehicle3D` (`use_input` 0).
`Police3D::drive_car` (u8, default 0, takes a former pad byte so the size stays 68) makes `kit3d_wanted` write the car's
`throttle`, `steer`, `brake` and `handbrake` every tick: Pursue and Attack aim at the target's position, Search at the last
known place, Idle stops. Steering is the signed angle from the car's heading to the point, divided by `max_steer`; throttle
falls with the steering lock and is capped at 0.4 inside 12 m; inside 4 m the car brakes to a stop, which is within
`arrest_range` of an offender who stands beside it only if the car stopped close, so a car and a foot unit together is the
normal pairing. `aim` does not turn a car. A roadblock is a `Vehicle3D` with `drive_car` 0 left across the road (no
engine work). Limits: no path following (a wall between the car and the target stops it; `Vehicle3D` has no nav), no
reversing out of a dead end, no ramming logic. Test: "a police car steers and drives at the target": right, left, stops at the
target, `drive_car` 0 leaves the car alone.

Step 4 (police cars on the road network). `Police3D::drive_car` 2 (same byte, so the size stays 68) makes the car follow the
`RoadNetwork3D`: `road_route` from the lane under the car (going its way, connectors allowed, within 30 m) to the lane nearest the
target or last known place (within 80 m). The aim point is 10 m plus half the speed down the route; steering and throttle are those
of step 3, but slowing (inside 12 m) and stopping (inside 4 m) use the route metres still to go. No lane near the car or the target:
it drives straight as for 1. The lanes under the car and nearest the target are looked up afresh every tick (two grid queries), so the aim, the distance to go and
the stop all follow the target at once; only the A* is cached per car, keyed by (the car's lane, the goal's lane), so it is a pure
cache: a run resumed from a save (the cache is not saved) drives exactly like one that never stopped. The car's lane is the one going
its way (a junction connector counts as a lane, so a car crossing a junction routes from the connector). Tests: "takes the roads to a
target round the corner" (a kinematic car drives an L to the target and parks; the straight car cuts across), "whose target is
behind it ... keeps on to turn round", "replays to the same state", "resumed with no route cache drives like one that never
stopped", "follows a target that moves to another lane at once".
Limits: the car ignores traffic lights and other cars, does not reverse out of a dead end or a jam (follow-up) and may take the
oncoming lane.

Step 5 (unstuck and no jitter). `Police3D` gains `stuck_time` and `reverse_left` (engine-written, saved; 68 to 76 bytes, appended, old scenes
load with 0). A driven car (`drive_car` 1 or 2) that pushes the throttle for 2 s while its `Vehicle3D::speed` stays under 0.5 m/s
reverses for 1.5 s (throttle -0.6, wheel against the way it wants to go), then tries again; a car held by the brake near its target is
not counted. A drive test over the L's junction (a kinematic car that reports its speed) found a real wheel flip: when the route ended
within the look-ahead the aim switched between the last lane point and the goal itself, which can be 20 m off the road, and the wheel
jumped between lock and straight every tick (24 sign flips). The aim now stays on the route's last point, the road spot nearest the
goal. Tests: "backs out and tries again" (jammed car: about 2 s forward, 1.5 s reverse, forward again), "a moving police car is not
mistaken for a stuck one", "does not jitter through a junction" (biggest tick-to-tick wheel change under 0.5, at most 6 sign flips).
Limits: the reverse is blind (no check of what is behind), a jam that lasts keeps cycling, a car still ignores traffic lights.

## Open items

- Cars that follow a road path or reverse out of a dead end, spawning fresh units (needs a prefab/station), and a handcuff animation / UI prompt (the event is the hook).
- An offender in a car is arrested the same way once it is stopped within reach; there is no pull-over.
