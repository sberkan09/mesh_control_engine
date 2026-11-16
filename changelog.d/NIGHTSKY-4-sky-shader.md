- Render: the sky pass draws `render::NightSky` (`Scene3D::set_night_sky()`): a moon disk of `moon_cos_size` around
  `moon_dir`, `moon_intensity` bright and tinted by `moon_color`, shaded as a sphere with the terminator of
  `moon_phase` (lit on the side the moon travels toward along the sun's default arc while it waxes), a soft limb, a
  faint earthshine and halo; and a procedural star field (an integer cell grid on the cube faces of the view
  direction, hashed for presence by `star_density` scaled by each cell's solid angle, so the stars are even over the
  sky, and for position, magnitude and tint; about 1.5 pixel points sized from the pixel footprint; a hash-phased twinkle of `star_twinkle` on `star_time`, wrapped in f64 on the CPU to 60 s with
  whole-cycle rates, so the clock never costs precision), scaled by `night_fade`, above the horizon only. Drawn under
  the cloud layer (a deck dims or hides it, as it does the sun disk) and under the fog. New pass uniforms `p_night`,
  `p_night2`, `p_moon`, `p_moon2` (written only when the moon or the stars are on). Main pass only (probes, IBL and
  GI captures stay without it). Moon intensity and star density 0, `enabled` false, a night fade of 0, the Low tier's
  stars and `r.nightsky 0` draw byte-identical frames (every existing golden unchanged). New goldens `night_sky` and
  `night_sky_crescent`. (M-NIGHTSKY 14d)
