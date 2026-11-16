- Rain splashes, M-RAINFX step 7f (the rest of 7c): the optional `RainSplash` component (`rate` 1, `height_m` 0.15,
  `life_seconds` 0.25, `radius_m` 25, `seed`) on an entity with a `ParticleEmitter3D` fires one seeded burst per tick
  at a random point within `radius_m` of the primary camera while it rains (`round(4 * intensity * rate)` particles).
  The system `kit_rain_splash` runs after `kit_precipitation`, creates no entities and saves its burst counter.
  Scenes without a `RainSplash`, in clear weather or without a camera are unchanged. Guide 9.19c.
