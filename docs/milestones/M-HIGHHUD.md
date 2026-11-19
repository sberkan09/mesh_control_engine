# M-HIGHHUD: the HUD tells high beam from normal beam

**Status:** done (2026-11-19), one PR. **Owner: routine 2 thread (the M-VISUAL line).** Follows [M-HIGHBEAM](M-HIGHBEAM.md) and
[M-HUDLIGHTS](M-HUDLIGHTS.md). No Linear issues (free limit); this record is the tracker.

**What.** `DriveReadout3D.lights` is 2 while the car's headlight shines on `HeadlightMode::High` (1 normal, 0 off, -1000 on foot). The
09_openworld HUD has two widgets in the same slot: "LIGHTS" (visible for 0.5 to 1.5) and a blue "HIGH BEAM" (visible for 1.5 to 2.5).

**Test.** `tests/kit/test_drive_readout.cpp`: the lights case now checks 2 on high beam and back to 1 on On.
