# M-HAZARD: hazard lights

**Status:** done (2026-11-19), one PR. **Owner: routine 2 thread (the M-VISUAL line).** Follows [M-TURNSIGNAL](M-TURNSIGNAL.md). No Linear
issues (free limit); this record is the tracker.

**What.** Kit `VehicleHazards3D` (4 bytes, `on`, saved) on a car: the `Hazard` action, pressed by the seated player driving it, toggles
`on`. While it is 1, every `TurnSignal3D` lamp of that car blinks regardless of steering or speed, so both sides flash together.

**Sample.** `gen_assets.py` adds the `Hazard` action (K; the pad's right-stick click, which Melee also uses, but Melee is blocked while
seated) and `VehicleHazards3D` on the cars the player can take.

**Test.** `tests/kit/test_vehicle_extras.cpp`: "the Hazard action toggles both signals blinking, standing still".

**Not done.** Front signals (the HUD indicator: [M-HAZARDHUD](M-HAZARDHUD.md)); (hazards switching on by themselves after a crash: [M-CRASHHAZARD](M-CRASHHAZARD.md)).
