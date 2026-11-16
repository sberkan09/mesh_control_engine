- Render: the main pass draws `render::WaterSurface` (`Scene3D::set_water()`, M-WATER 15c): a flat, camera-following
  square (16 x 16 cells, centre snapped to whole cells) at `level_y` (world height; the floating origin's y is taken
  off), drawn after the opaques and before the sky and the blended draws, depth-tested and depth-written. New shaders
  `kWaterVS` / `kWaterFS`; the sky's cloud, star and moon code moved into the shared `MCE_GLSL_SKY_LAYERS` (the sky pass
  is unchanged), which the water's reflection calls. Shading: the Gerstner normal of `kit::water_normal` (the kit's wave
  tables expanded on the CPU, the floating origin and the wrapped clock folded into one f64 phase per wave), each wave
  fading to flat under 8 pixels per wavelength, two small fixed detail ripples; Schlick fresnel (F0 0.02, times
  `reflectivity`) between the body tint and the reflected sky (gradient or texture at the `roughness` lod, clouds, moon
  and stars, horizon haze); a GGX glint from the frame's light whose width grows with the faded waves; the body from
  `shallow_color` to `deep_color` by the optical depth along the view ray to the opaques behind, read from the half-size
  prepass shared with SSAO (run for the water when nothing else ran it); shore foam within `foam_width_m` of that
  surface, in moving stripes broken by the crests; fog last. Without the prepass pipelines, and for the Low tier's flat,
  foamless plane, a distance tint and no foam. New pass uniforms `p_water` .. `p_water5` and `p_wave[8]` after
  `p_moon2` (PassU stays 1280 bytes). The pipeline is made on the first frame with water. Every existing golden
  unchanged on GL and SDL GPU; new goldens `water_calm`, `water_sea`, `water_night`; pixel tests in
  `test_render3d_water_gl`. (M-WATER 15c)
