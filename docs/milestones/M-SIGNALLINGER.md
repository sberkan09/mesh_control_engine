# M-SIGNALLINGER — the indicator outlasts the turn

Owner: rutin 2 (M-VISUAL), SBA-233. Builds on [M-TURNSIGNAL](M-TURNSIGNAL.md).

`TurnSignal3D.linger_s` (seconds, 0 = off) keeps the lamp blinking for that long after the steering straightens or the car
stops, so a short turn shows a readable signal. The timer restarts every tick the car steers toward the lamp's side, and the
hazard lights do not linger. The open-world cars use 1.2 s. Engine-written: `linger_left`. Test: `vehicle extras: a turn signal
keeps blinking for linger_s ...`.
