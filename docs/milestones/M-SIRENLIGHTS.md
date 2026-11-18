# M-SIRENLIGHTS — Flashing red and blue light bars on police cars (milestone record)

**Status:** done (2026-11-18), one PR. **Owner: routine 2 thread (the M-VISUAL line).** Closes the "no flashing sirens" gap of
M-OPENWORLD-GAPS and M-OPENWORLD-POLICE. No Linear issues (free limit); this record is the tracker.

**What.** Kit `LightFlasher3D` (44 bytes: `color_a`, `color_b`, `intensity`, `period_s`, engine-written `t`) in
`mce/kit/vehicle_extras.h`, handled by `kit_vehicle_extras`. With a `PointLight3D` on the same entity it shows `color_a` for the
first half of each period and `color_b` for the second, starting each half at `intensity` and fading to 30% of it, so it reads as a
strobe. Generic: any emergency vehicle or sign can use it.

**Sample.** `police_car` in `gen_assets.py` adds a "light bar" child (a `ChildOf` the car, on the roof) with a red and blue
`LightFlasher3D` (14, 0.5 s) and an 11 m radius `PointLight3D`. Every police car carries one, so each adds one local light to the city
while it exists.

**Tests.** `tests/kit/test_vehicle_extras.cpp`: red first, fades, blue in the second half, red again.

**Not done.** Light bars that flash only while the car chases; a lens mesh that glows.
