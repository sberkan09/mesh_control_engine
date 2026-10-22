- A moon with phases, M-NIGHTSKY step 14a: the optional `Moon` component (phase offset, cycle days, frozen phase,
  inclination, moonlight intensity, colour and shadows, disk size and intensity, star density, brightness and twinkle)
  and the pure `moon_phase`, `moon_illumination`, `moon_direction` and `moon_level` in `mce/kit/moon.h` turn the
  `WorldClock` day and hour and the weather into a phase, a direction and a 0..1 level. There is no system and nothing
  is stored; nothing reads it yet (14b to 14e), so existing scenes are unchanged. Guide 9.19j.
