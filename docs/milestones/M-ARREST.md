# M-ARREST — police arrest an offender who stands still

Status: step 1 and 2 done (2026-10-26). Own gameplay milestone (coordinator: pick the most valuable missing GTA-slice system; wanted level / police was one of the examples). M-OPENWORLD C3 listed arrests as an open item.

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

## Open items

- Police cars and roadblocks (police are on foot), spawning fresh units (needs a prefab/station), and a handcuff animation / UI prompt (the event is the hook).
- An offender in a car is arrested the same way once it is stopped within reach; there is no pull-over.
