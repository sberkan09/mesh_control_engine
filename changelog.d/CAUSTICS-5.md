- Caustics through the water, M-CAUSTICS step 52f: with the camera above the water the web is now drawn into the copy of
  the opaque frame that the water refracts (in place of the plain copy, before the water draws), so the water shows it
  exactly as it shows the floor: bent by the waves, weighted by its own Fresnel on the wave normal, its colour along the
  view path, its foam and its reflection. This fixes the light-blue fringe the real-GPU round r12 found where the water
  reflects a dark wall next to refracted sand, and the bright streaks on the shaded underwater sides of rocks (the old
  post pass weighted the light by a flat plane's Fresnel at the pixel's own floor point). Under the water the pass stays
  a post pass after TAA. No new target; `kCausticsFS` is smaller (`CausticsU` 10 vec4, the water's extinction and
  reflectivity gone), `kMeshFS`, `kTerrainFS`, `kWaterFS` and `PassU` untouched. The CPU twin's `caustics_term` no
  longer applies a water transmission (`caustics_transmission`, `caustics_through_water`, `caustics_water_sigma` and the
  `vol_surface` argument removed); golden `caustics` regenerated (gl, sdlgpu).
