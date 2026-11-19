# M-FRONTSIGNAL: turn signals at the front too

**Status:** done (2026-11-19), one PR. **Owner: routine 2 thread (the M-VISUAL line).** Closes the "front signals" gap of
[M-TURNSIGNAL](M-TURNSIGNAL.md) and [M-HAZARD](M-HAZARD.md). No Linear issues (free limit); this record is the tracker.

**What.** Data only: `car_visuals` in `gen_assets.py` adds a left and a right `TurnSignal3D` lamp at the front corners (z -2, the car
drives toward -z) next to the rear ones, so every car now blinks at all four corners and the hazard lights flash all four. No kit change.

**Cost.** Four disabled point lights per car instead of two; a lamp costs nothing while dark.

**Not done.** A glowing lens mesh; hazards switching on by themselves after a crash.
