- SSR on bumpy surfaces and moving hits, M-REFLECT2 steps 48d and 48e. A normal-mapped (or terrain-layer-mapped)
  rough surface now blurs its reflection more where the map tilts the normal: the gather radius adds
  `3 * (1 - dot(mapped, geometric))` to the GGX alpha (`ssr_bump_term`), scaled down under decals, puddles and snow;
  the blend weight keeps the material roughness, so a bumpy floor still reflects, and a surface at or below roughness
  0.1 keeps its single sharp tap. The reflection still follows the geometric normal for where it looks; the bump only
  widens it. Scenes without normal maps are byte-identical. When TAA, motion blur or depth of field already draws the
  velocity buffer, the SSR trace reads it at each hit, so a moving object's reflection shows where the object is
  instead of trailing a frame behind (the velocity pass then records before the trace; its GPU time moves from the
  TAA bucket to the SSR one). `r.ssr.motion 0` (new, default 1) returns the camera-only reprojection;
  `Forward3DStats::ssr_motion` reports when the velocity was read.
