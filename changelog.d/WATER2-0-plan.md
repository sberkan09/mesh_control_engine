- M-WATER2 plan (docs only): `docs/milestones/M-WATER2.md` plans the water's second pass: it refracts what lies under it
  (this frame's opaque picture, copied before the water draws through the existing transmission copy, bent by the wave
  normals, depth-checked so nothing in front leaks in, and absorbed per channel as the underwater camera does), reflects
  on-screen geometry through a short screen-space trace in the water shader that falls back to today's sky reflection,
  and flows through an opt-in kit `WaterFlow` (direction and speed) that carries the waves, the ripples and the Luau
  `water.height_at` along; cvars `r.water.reflect` and `r.water.refract`, a `tour_river` scene in `kit3d_tour`; steps
  49a to 49h. Follow-up of M-WATER, M-UNDERWATER and M-REFLECT2.
