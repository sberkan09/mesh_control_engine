- Camera look fields, M-VISUAL3 step 3a (SBA-199): `Environment3D` gains `vignette`, `vignette_smoothness`,
  `film_grain`, `grain_size`, `chromatic_aberration`, `lens_dirt` / `lens_dirt_intensity` and `tonemapper`
  (0 ACES fit, 1 AgX, 2 Khronos PBR Neutral); `Camera3DComponent` gains `focus_distance` (0 = the environment's
  `dof_focus_distance`). They reach `PostFx::lens` and `PostFx::tonemapper`; the tonemap shader that draws them is
  step 3b, so a frame does not change yet. Worlds that carry an `Environment3D` hash differently (the struct grew).
