- Rain ripples draw, M-RAINFX step 7b: `Scene3D::set_ripples()` with a strength above 0 (and `enabled`) rings the
  puddles: a world-anchored grid of 0.5 m cells (periodic over 256 m like the puddles, so a floating-origin rebase
  keeps the drops in place), `density` the share of cells holding a drop, each drop landing at a hashed point once
  per hashed period (0.75 to 1.5 s at `speed` 1) and sending out two concentric rings that grow to 0.4 m and fade.
  The rings tilt the water's normal (and roughen it a little at their crests) and are multiplied by the puddle mask:
  no wetness, no puddles or the Low tier draw the ripple-less frame byte for byte, and ground outside the puddles is
  untouched. Snow over a puddle stills it. The clock is the scene's (`Scene3D::set_time`), never the wall clock.
  Main pass only: reflection probes and the irradiance volume capture still water. Every existing golden unchanged;
  new golden `ripple_plaza`. The `unit01` and noise-cell clamps are shared between `apply_quality` and the pass
  upload (`src/engine/render/src/unit_clamp.h`).
