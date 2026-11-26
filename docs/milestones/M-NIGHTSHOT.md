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

## Step 2: the wet variant (2026-11-26)

`MCE_NIGHT_SHOT=wet` (or `NIGHT=wet tools/night_shot.sh`) adds rain: the Weather starts on Rain with a 0.05 s fade, so the roads are wet by
the capture. `notes/headlight-seat-wet-diff.png` (same layout): 5949 pixels differ by more than 10 against 5819 dry, so the paint
term is unchanged by rain. That is by design: the retro term reads only the material's `retro_reflect` and the light, not the wet-surface
state, and the wet-road darkening and puddles act on the road, not on the markings. The wet frame shows the rain's fog and the darker
asphalt, with the same bright lane dashes and centre line. A wet-dependent retro (beads under a water film throw back less) would be a
material-model change in render core and is not planned.
