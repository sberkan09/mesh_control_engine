- Cirrus kit component and Luau, M-CIRRUS step 53c: the opt-in `Cirrus` component (`mce/kit/cirrus.h`, one per scene,
  registered by `install_weather`, no system, nothing stored: `strength`, `fair_coverage`, `front_coverage`, `density`,
  `scale_m`, `stretch`, `speed_scale`, `height_m`) and the pure helper `cirrus_coverage(cirrus, weather, day_fraction)`:
  the cover goes from fair toward front while a change of weather is pending or fading in, drops under a heavy overcast,
  and varies slowly from day to day from a hash of the `WorldClock` day (no `sin()`; `strength` 0 is exactly 0, NaN falls
  back to the defaults). `Kit3DExtract` feeds `render::Cirrus` from it (the first `WindDirection`'s heading, the drift
  from `Weather.wind * speed_scale`) through `apply_quality`, so the Low tier and `r.cirrus 0` switch it off; a scene
  without a `Cirrus` renders as before. Luau: `weather.set_cirrus{strength=, fair_coverage=, front_coverage=, density=,
  scale=, stretch=, speed_scale=, height=}`, the getters `weather.cirrus_strength()` and so on, and the read-only
  `weather.cirrus_coverage()` (named errors, each value clamped to its range, nothing written on an error), typed in the
  Luau definitions; the user guide (8.5, 27.3).
