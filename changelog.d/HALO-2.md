- M-HALO kit component and Luau (step 54c): the opt-in `Halo` component (`mce/kit/halo.h`, one per scene, registered by
  `install_weather`, no system, nothing stored: `strength`, `sun_dogs`, `moon`, `dispersion`) and the pure helper
  `halo_level(halo, cirrus_cover)` (`strength` times a smoothstep of the cirrus cover, 0 up to 0.1 and full from 0.5;
  `strength` 0 is exactly 0, NaN falls back to the defaults). `Kit3DExtract` feeds `render::Halo` from it every frame
  through `apply_quality`, so the Low tier and `r.halo 0` switch it off; a scene without a `Halo`, a `Cirrus` or a
  `Weather`, or with an equirect sky texture, renders as before. Luau: `weather.set_halo{strength=, sun_dogs=, moon=,
  dispersion=}`, the getters `weather.halo_strength()` and so on, and the read-only `weather.halo_level()` (named errors,
  each value clamped to 0..1, nothing written on an error), typed in the Luau definitions; the user guide (8.5, 27.3).
