- M-HALO sky shader (step 54b): the sky now draws `render::Halo` through the cirrus layer: a 22 degree ring round the
  sun with a sharp inner edge, a soft glow outside and a faint colour fringe (red inside, blue outside; `dispersion`),
  two sun dogs on the sun's horizon line 22 degrees from it (`sun_dogs`; brightest at a low sun, gone above about 60
  degrees) and a faint ring round the moon by night (`moon`). It shows only where the cirrus streaks are (a clear sky,
  a gap between the streaks or cirrus coverage 0 shows none), sits behind the low cloud layer and the fog, and the water
  reflects it. The ring's edge is never thinner than 1.5 pixels, so it does not crawl. `PassU` takes its last free
  `vec4` (`p_halo`, size unchanged; the next sky feature needs a growth seam). The CPU twin is `mce/render/halo.h`.
  Strength 0 (the default), `enabled` false, the Low tier and `r.halo 0` draw every frame exactly as before; the IBL
  and reflection probes do not see the halo.
