- M-CLOUDSHADOW plan (docs only): `docs/milestones/M-CLOUDSHADOW.md` plans drifting cloud shadows on the ground. A post
  pass on the HDR frame rebuilds each pixel's world position from the prepass depth, follows the sun to the sky layer's
  cloud plane and reads the sky's own noise and wind drift there, so the patches of shade match the clouds overhead and
  move with them; the lit shaders are not touched. Opt-in through a kit `CloudShadow3D` (needs a `CloudLayer`), cvar
  `r.cloud_shadows`, off on Low; scenes without it render as today. Steps 51a to 51f. Follow-up of M-CLOUDS and
  M-SUNSHAFTS.
