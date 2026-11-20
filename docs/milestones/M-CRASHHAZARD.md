# M-CRASHHAZARD: hazards switch on by themselves after a crash

**Status:** done (2026-11-20), one PR. **Owner: routine 2 thread (the M-VISUAL line).** Closes the last gap of [M-HAZARD](M-HAZARD.md). No
Linear issues (free limit); this record is the tracker.

**What.** `VehicleHazards3D::crash_speed` (struct 4 to 8 bytes; 0 = never): a `ContactBegin3D` that involves the car with an
`approach_speed` at or above it sets `on` to 1, so a car that hits a wall or another car starts flashing its hazards. The driver can still
switch them off with the `Hazard` action. The 09_openworld player cars set 8 m/s.

**Test.** `tests/kit/test_vehicle_extras.cpp`: "a hard enough crash switches the hazards on by itself" (0 never, a bump below, a hit between
other things, a crash, switching off).

**Not done.** Hazards that go off by themselves after the car has stood still for a while.
