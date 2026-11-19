# M-HAZARDHUD: the HUD shows the hazard lights

**Status:** done (2026-11-19), one PR. **Owner: routine 2 thread (the M-VISUAL line).** Closes the "HUD indicator" gap of
[M-HAZARD](M-HAZARD.md). No Linear issues (free limit); this record is the tracker.

**What.** `DriveReadout3D.hazard` (struct 16 to 20 bytes): 1 while the car the rider sits in has `VehicleHazards3D.on`, 0 when off or
absent, -1000 on foot. The 09_openworld HUD shows an amber "HAZARD" under "LIGHTS" while it is 1.

**Test.** `tests/kit/test_drive_readout.cpp`: "hazard reads 1 while the car's hazard lights are on, 0 otherwise, off on foot".
