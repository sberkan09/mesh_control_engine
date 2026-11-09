- M-GI2 plan (docs only): `docs/milestones/M-GI2.md` plans the irradiance volume's second pass: each probe records how far
  it sees in six directions (a geometry-only capture pass that also draws back faces) and how much of what it sees is the
  inside of something, so `gi_irradiance2` weighs probes by a Chebyshev visibility test and switches off probes buried in
  geometry (no more light leaking through walls thinner than the probe spacing, no more darkening from buried probes);
  probes a little way inside a solid can be moved out of it, and a volume can follow the camera in whole cells, keeping
  the probes it already captured. Opt-in through a kit `GiVisibility3D` component, cvar `r.gi.visibility`; scenes
  without it render as today. `samples/09_openworld` switches over. Steps 50a to 50i. Follow-up of M-VISUAL step 6 and
  M-OPENWORLD-LIGHT.
