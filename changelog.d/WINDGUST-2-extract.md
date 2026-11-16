- Wind gusts, M-WINDGUST step 12b: `Kit3DExtract` reads the optional `WindGust` (read only) so foliage sway uses
  `effective_wind` (sway bit) and the snow lee bias becomes `clamp(wind_bias * (1 + strength * gust), 0, 1)` (snow bit).
  Cloud drift is not gusted: the render side computes the offset as `speed * time`, so a speed change would jump the
  layer. A scene without a `WindGust` (or with the bit clear, `enabled` 0 or a zero level) extracts bit-identically.
  Guide 9.19h.
