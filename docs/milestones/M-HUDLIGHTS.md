# M-HUDLIGHTS — A LIGHTS indicator on the driving HUD (milestone record)

**Status:** done (2026-11-18), one PR. **Owner: routine 2 thread (the M-VISUAL line).** Follow-up of M-OWHORN ("no HUD icon").
No Linear issues (free limit); this record is the tracker.

**What.** `DriveReadout3D` (M-DRIVEHUD) gets `lights` (16 bytes now): 1 while the seated car's `SpotLight3D` is on (`enabled` and
`intensity` above 0), 0 when it is off or the car has none, -1000 on foot like the other fields. `kit_drive_readout` fills it from
the car, so Auto (the day cycle) and the driver's On / Off both show.

**Sample.** `hud.mcui` (from `gen_assets.py`) has a "LIGHTS" text under the gear, shown with `range` [0.5, 2] so it appears only
while the lights are on, in a car.

**Tests.** `tests/kit/test_drive_readout.cpp`: lights 0 without a spot, 1 with one on, 0 when it is switched off, off on foot.

**Not done.** ~~A high-beam state~~ (done: [M-HIGHBEAM](M-HIGHBEAM.md)).
