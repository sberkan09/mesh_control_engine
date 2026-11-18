# M-HORNSCARE — Pedestrians jump away from a honking car (milestone record)

**Status:** done (2026-11-18), one PR. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-OWHORN ("pedestrians do not
react to the horn"). No Linear issues (free limit); this record is the tracker.

**What.** `kit_crowd` treats a car that honked this tick (`VehicleHorn3D.since_s` is 0 on the tick it honks, one `dt` on the next) as
a short scare: a `Pedestrian3D` within `kHornRadius` (12 m) of it runs away from the car for `kHornFlee` (2 s), against 30 m and 5 s
for a shot. It uses the same `Fear` mechanism as shots, crashes and run-down cars, so the walker turns to the end of its pavement
edge farther from the car, ignores the light while it runs, and the panic spreads to close neighbours (`kPedestrianPanicRadius`).
Nothing new is saved. No component or header change beyond `crowd.cpp` including `vehicle_extras.h` (6 translation units).

**Tests.** `tests/kit/test_crowd.cpp`: a walker 6 m from a honking car runs and calms down after the 2 s; one 30 m away does not.

**Not done.** Traffic cars braking or swerving for a horn.
