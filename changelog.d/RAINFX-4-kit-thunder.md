- Delayed thunder, M-RAINFX step 7e (`ThunderDelay` only; `RainSplash` moves to 7f): the optional `ThunderDelay` component
  (`speed_mps` 343, `volume` 1, `clip`) turns each `LightningStruck` into a one-shot `PlaySfx` that starts
  `ceil(distance / speed_mps / dt)` ticks later, quieter with distance (`volume / (1 + d / 100 m)`, floor -60 dB).
  Up to 8 strikes in flight (a ninth is dropped), saved so a save mid-storm resumes on the same state hash. Scenes
  without a `ThunderDelay` sound as before. Guide 9.19a.
