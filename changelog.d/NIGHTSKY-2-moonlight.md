- Moonlight in the 3D extract, M-NIGHTSKY step 14b: with a `Moon` (and a `WorldClock`), `Kit3DExtract` hands the scene's
  one directional light to the moon at night (its direction, `light_color`, `light_intensity` scaled by the moon level,
  `light_shadows`, plus a small cool ambient lift) once the moon's weighted contribution beats the sun's; only the
  extract's local light changes, `Environment3D` is untouched, and scenes without a `Moon`, with `enabled` 0, no clock or
  a zero level render as before. Pair the `Moon` with a `DayCycle` so the hand-off happens while the sun is dim.
