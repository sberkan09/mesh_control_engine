# M-TURNSIGNAL: blinking turn signals

**Status:** done (2026-11-19), one PR. **Owner: routine 2 thread (the M-VISUAL line).** Follows [M-TAILLIGHT](M-TAILLIGHT.md). No Linear
issues (free limit); this record is the tracker.

**What.** Kit `TurnSignal3D` (36 bytes: `color`, `intensity`, `period_s`, `threshold`, `side`, engine-written `t`) in
`mce/kit/vehicle_extras.h`, handled by `kit_vehicle_extras`. On a child lamp of a car (a `ChildOf` the car with a `PointLight3D`): while the
car steers toward `side` (-1 left, 1 right) by more than `threshold` (its own `steer`, or the player's Move.x, whichever is larger) and
rolls faster than 0.5 m/s, the lamp blinks amber, on for the first half of each `period_s` (0.7 s) and off for the second.

**Sample.** `car_visuals` in `gen_assets.py` adds a left and a right signal lamp at the rear corners of every car (traffic, police and the
player's), two more disabled point lights per car; a lamp costs nothing while dark.

**Test.** `tests/kit/test_vehicle_extras.cpp`: "a turn signal blinks only while the car steers toward its side and rolls".

**Not done.** (Front signals: [M-FRONTSIGNAL](M-FRONTSIGNAL.md); hazard lights: [M-HAZARD](M-HAZARD.md).); a glowing lens mesh.
