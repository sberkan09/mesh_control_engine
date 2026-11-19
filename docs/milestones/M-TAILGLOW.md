# M-TAILGLOW: dim tail lamps while the headlights are on

**Status:** done (2026-11-19), one PR. **Owner: routine 2 thread (the M-VISUAL line).** Follows [M-TAILLIGHT](M-TAILLIGHT.md). No Linear
issues (free limit); this record is the tracker.

**What.** `BrakeLight3D::headlight_idle` (struct 48 to 52 bytes): when the car is neither braking nor reversing and its own `SpotLight3D`
(the headlight, driven by the day cycle or the Lights action) is on, the tail lamp shows this intensity instead of `idle`. 0 leaves
the old behaviour. Braking and reverse still win.

**Sample.** The 09_openworld car tail lamp sets 1.5, so night cars and cars with the headlights switched on show dim red rear lamps.

**Test.** `tests/kit/test_vehicle_extras.cpp`: "a tail lamp glows at `headlight_idle` while the car's headlights are on".

**Not done.** Separate indicator / hazard lamps; a glowing lens mesh.
