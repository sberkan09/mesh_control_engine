# M-DRIVEHUD: speed and gear on the HUD while driving (2026-10-31)

Kit: `DriveReadout3D` (`mce/kit/drive_readout.h`), system `kit_drive_readout` (FixedUpdate_Pre, after `kit3d_vehicles`). On the entity that holds a `Rider3D` it copies, while the rider is seated, the car's speed (`kmh`, always >= 0), `gear` (-1 reverse, 0 neutral, 1.. forward) and `rpm` from `Vehicle3D`. On foot all three read `kDriveReadoutOff` (-1000), so a HUD widget with a `range` starting above that is hidden, the same trick as the wanted stars. Floats, so every bind format works.

Sample (`09_openworld`): `DriveReadout3D` on the Player; `hud.mcui` gets "N KM/H" and "GEAR n" at the bottom right, above the ammo (`range [-0.5, 1000]` and `[-1.5, 20]`; an initial `value` of 0 inside the range, as the cook requires).

Tests: `tests/kit/test_drive_readout.cpp` (on foot, seated, reversing, leaving), the MCP openworld test checks the component. Limits: no rpm needle (only speed and gear text), km/h only (no mph switch), the gear shows -1 for reverse instead of "R".
