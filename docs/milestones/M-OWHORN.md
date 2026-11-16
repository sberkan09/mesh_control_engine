# M-OWHORN: horn and headlights for the player's car (2026-10-31)

Kit (`mce/kit/vehicle_extras.h`, `vehicle_extras.cpp`, installed from `install_stock`): `VehicleHorn3D` (clip, gain, `min_gap_s`, `attenuation_id`, `honk`, engine-written `honks`) and `VehicleLights3D` (`HeadlightMode` Auto, On, Off). System `kit_vehicle_extras` runs in `FixedUpdate_Pre` after `kit_timed_light`. Only a car whose driver is a seated `Rider3D` with `use_input` reads the `Horn` and `Lights` actions, so traffic and police cars never respond. A honk pushes one 3D `audio::PlaySfx` at the car. On and Off overwrite the day cycle's write on the car's own `SpotLight3D`. The design note is section K of [M-OPENWORLD-GAPS](M-OPENWORLD-GAPS.md); vehicle, seat, traffic and timed-light files are untouched.

Sample (`gen_assets.py`): a synthesized `audio/horn.wav`, `Horn` (G, stick press) and `Lights` (L, D-pad right) actions, the two components on the cars the player can take.

Tests: `tests/kit/test_vehicle_extras.cpp` (honk per press and gap, no honk outside the car, `honk` field, lights cycle), `test_mcp_openworld` (a seated player honks once and turns the lights on). Golden component count 178 to 180.

Limits: pedestrians and traffic do not react to the horn; no brake or tail lights, no high beam, no HUD icon; the horn is a plain synthesized tone.
