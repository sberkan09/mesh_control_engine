- Camera look in the tonemap pass, M-VISUAL3 step 3b (SBA-199): the `Environment3D` fields from 3a now draw. `vignette`
  darkens the corners in linear light, `chromatic_aberration` fringes red and blue toward the edges, `film_grain`
  adds animated mid-tone grain after the grade (FXAA reads its luma before it), `lens_dirt` is lit by bloom, and
  `tonemapper` 1 / 2 select AgX / Khronos PBR Neutral instead of the ACES fit. All off by default: a scene that asks for
  none draws the same frame. The grain and the fringe follow `access.motion_scale`. New goldens `filmic_lens` and
  `tonemap_agx`.
