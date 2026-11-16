- Lighting seam (M-OPENWORLD-LIGHT follow-up): `Environment3D.auto_exposure_min` / `auto_exposure_max` clamp the auto
  exposure factor (defaults 0.03 / 32, PostFx's, so scenes are unchanged), and `render::Atmosphere::sky_sun_direction` /
  `sky_sun_light` let a sun other than the DirectionalLight light the sky (a moon holding the light at dusk); unset, the
  light is the sky's sun as before.
