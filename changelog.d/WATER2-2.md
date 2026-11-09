- M-WATER2 49b-49d: the water reflects and refracts the scene. With the depth read on (waves or foam, and the prepass
  pipelines) and the camera above the level, the main pass breaks before the water and copies the opaque frame into the
  transmission copy (`trans_src_`, bank 7); `kWaterFS` then shows what lies under the water through it (bent by the wave
  tilt, only where the surface found there is behind the water and under its level, absorbed per channel with
  `underwater_sigma`'s formula, ending at the old tint in deep water) and reflects on-screen geometry with a 24-step
  screen-space march against the prepass depth, fading into the sky reflection (a miss is the sky bit for bit). Cvars
  `r.water.refract` and `r.water.reflect` (default 1; 0 = the old water, no pass break); `Forward3DStats::water_refract`
  / `water_reflect` say what ran. Pure helpers in `mce/render/water_optics.h` (`water_refract_offset`,
  `water_refract_accept`, `water_reflect_fade`, `water_flow_cycles`). The six water goldens render with both cvars at 0
  (unchanged files); the new look is pinned by `test_render3d_water_optics_gl`.
