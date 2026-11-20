# M-HORNFLASH — a honk flashes the headlights

Owner: rutin 2 (M-VISUAL), SBA-230. Builds on [M-OWHORN](M-OWHORN.md) and [M-HIGHBEAM](M-HIGHBEAM.md).

`VehicleLights3D.flash_s` (seconds, 0 = off) makes the spot go High (intensity and radius times `high_scale`) for that long
after each honk, whatever the mode: even in `Auto` by day with the lamp off, or with the driver's `Off`. When it ends the
beam goes back to what the mode and the day cycle say. It reuses the High-beam bookkeeping, so it does not compound.
The open-world cars use 0.25 s. Test: `vehicle extras: a honk flashes the beam High ...`.
