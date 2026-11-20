# M-BRAKESTROBE — a hard stop strobes the tail lamp

Owner: rutin 2 (M-VISUAL), SBA-231. Builds on M-TAILLIGHT and [M-TAILGLOW](M-TAILGLOW.md).

`BrakeLight3D.strobe_decel` (m/s², 0 = off) makes the lamp strobe at `strobe_hz` flashes a second for 0.8 s whenever the
car brakes and loses at least that much speed per second (an emergency stop signal). A gentle stop keeps the steady
brake light; when the strobe times out the lamp is steady again. The open-world cars use 8 m/s². Engine-written:
`strobe_left`, `prev_speed`. Test: `vehicle extras: a hard stop strobes the tail lamp ...`.
