# M-HIGHBEAM: high-beam headlights

**Status:** done (2026-11-19), one PR. **Owner: routine 2 thread (the M-VISUAL line).** Closes the "high-beam state" gap of
[M-HUDLIGHTS](M-HUDLIGHTS.md). No Linear issues (free limit); this record is the tracker.

**What.** `HeadlightMode` gains `High` (appended, so saved values keep their meaning). The `Lights` action now cycles Auto, On, High, Off.
`VehicleLights3D` gets `high_scale` (1.5; intensity and radius are multiplied) and the engine-written `base_radius` / `base_intensity`
(the normal beam, restored when the mode leaves High; struct 4 to 16 bytes). The 09_openworld HUD "LIGHTS" readout reports any lit
beam as 1, so it does not change.

**Test.** `tests/kit/test_vehicle_extras.cpp`: the cycle test now checks High (1.5 times, not compounding) and the restore on Off.

**Not done.** Dazzling other drivers. (The HUD value that tells High from On: [M-HIGHHUD](M-HIGHHUD.md).)
