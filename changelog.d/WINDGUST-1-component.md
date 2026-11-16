- Wind gusts, M-WINDGUST step 12a: the optional `WindGust` component (strength, gap/rise/fall/peak ranges, seed,
  `affects` mask, integer gust state with explicit pads) and the system `kit_wind_gust` (after `kit_wind_direction`)
  produce a deterministic, saved gust level from the seed; the pure `effective_wind(Weather, WindGust*, bit)` in
  `mce/kit/wind_gust.h` returns `Weather.wind` bit-identically unless a gust applies. `Weather` is never written and no
  consumer reads the gust yet (12b, 12c), so existing scenes are unchanged. Guide 9.19h.
