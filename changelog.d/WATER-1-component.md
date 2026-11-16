- An opt-in water surface as data, M-WATER step 15a: the optional `Water` component (level, extent, wave count,
  amplitude, wavelength, steepness, wind direction and follow, speed, shallow and deep colour, depth scale, reflectivity,
  roughness, foam) and the pure `water_wave_set`, `water_height`, `water_normal` and `water_phase_speed` in
  `mce/kit/water.h` give a Gerstner wave height and normal at any position and time. There is no system and nothing is
  stored; nothing reads it yet (15b to 15d), so existing scenes are unchanged. Guide 9.19k.
