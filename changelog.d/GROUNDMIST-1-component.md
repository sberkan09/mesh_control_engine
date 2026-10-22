- Morning mist, M-GROUNDMIST step 13a: the optional `GroundMist` component (strength, peak hour, rise and burn-off
  hours, base humidity, wet gain, weather damp, volumetric density, thickness, base height, exp2 fallback density,
  colour) and the pure `mist_level(GroundMist, hour, Weather*)` in `mce/kit/ground_mist.h` turn the hour of day and the
  wetness left by rain into a 0..1 level. There is no system and the level is not stored; nothing reads it yet (13b, 13c),
  so existing scenes are unchanged. Guide 9.19i.
