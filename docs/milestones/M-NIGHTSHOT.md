# M-NIGHTSHOT: the driver-seat night shot of the retro-reflective road paint

Follow-up of [M-HEADLIGHT](M-HEADLIGHT.md), added 2026-11-25, owner: the M-VISUAL routine 2 thread. Content and tooling only; no engine change.

M-HEADLIGHT's first capture used the default camera and showed 127 pixels of difference (retro 0.8 against 0.0). The eye has to sit near
the light's axis for glass-bead paint to read as bright, so this milestone adds a capture variant where it does.

## What it is

- `MCE_NIGHT_SHOT=1 python3 gen_assets.py` writes the city at 22:00 with the player on the avenue's right lane and a lit parked car two
  metres behind the camera, so the headlights and the eye share an axis. The committed content is untouched (the variable is off by default).
- `samples/09_openworld/tools/night_shot.sh <out.png> [retro]` regenerates with the variable, captures 12 frames headless and restores the
  committed content. `retro 0` is the baseline.

## Result

`notes/headlight-seat-diff.png` (top left retro 0, top right retro 0.8, bottom the difference x4): 5819 pixels differ by more than 10 (14048
by any amount, max channel sum 114), against 127 from the default camera. The yellow centre line and the white lane dashes in the lit cone
are visibly brighter; the paint outside the cone is unchanged.

## Not done

A wet-road variant (WetSurfaces on) is the next visual check, since wet paint changes how much the beads throw back.
