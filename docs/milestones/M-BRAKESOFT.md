# M-BRAKESOFT — the brake light follows the pedal

Owner: rutin 2 (M-VISUAL), SBA-232. Builds on [M-BRAKESTROBE](M-BRAKESTROBE.md).

`BrakeLight3D.soft_scale` (0..1, 1 = off) shows `braking * (soft_scale + (1 - soft_scale) * pressure)` while the car
brakes, where pressure is the larger of the car's `brake` and `handbrake` (a held trigger counts as it reads), and 1 when the
throttle works against the way the car rolls. A feather touch glows dimmer, a full press is as bright as before. The
open-world cars use 0.5. Test: `vehicle extras: a light touch on the brake glows dimmer ...`.
