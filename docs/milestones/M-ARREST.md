# M-ARREST — police arrest an offender who stands still

Status: step 1 done (2026-10-26). Own gameplay milestone (coordinator: pick the most valuable missing GTA-slice system; wanted level / police was one of the examples). M-OPENWORLD C3 listed arrests as an open item.

## Why

Police could chase and shoot, never catch: a wanted player had only two outcomes, escape or die. In the GTA-like slice a low-level crime that ends with the player giving up is the normal "busted".

## What

Step 1. `Police3D` gains `arrest_range` (2.5 m), `arrest_time` (2 s), `arrest_speed` (1.5 m/s), `arrest_max_level` (1) and the engine-written `arrest_progress`. In `kit3d_wanted`, a unit that sees the offender within range, at a level of
`arrest_max_level` or below, with the offender moving no faster than `arrest_speed` (measured from its position last tick, kept in the system's state), builds progress; it falls back at the same rate otherwise. At `arrest_time` the unit
pushes `Arrested3D { offender, by, position }` and calls `clear_wanted`; the other units go Idle on the next tick. What the arrest costs the player is the game's, on that event. Levels above `arrest_max_level` keep the old behaviour
(chase, shoot). `Police3D` goes from 48 to 64 bytes (a saved-format change by appended fields; old scenes load with the defaults).

Test: `tests/kit/test_wanted3d.cpp` ("a unit arrests an offender who stands still within reach at a low level"): arrested after 2 s of standing still; no arrest when the offender keeps moving, is 5 m away, is at level 2, or the unit has `arrest_range = 0`.

## Open items

- Police cars and roadblocks (police are on foot), reinforcements as the level rises, and a handcuff animation / UI prompt (the event is the hook).
- An offender in a car is arrested the same way once it is stopped within reach; there is no pull-over.
