- Caustics pass, M-CAUSTICS step 52b: with `PostFx::caustics.enabled` (after `apply_quality`), a drawn water plane
  (`WaterSurface` on) and the sun at least about 1 degree up, Forward3D adds the sun's moving web of refracted light to
  the opaque pixels below the water plane (a sea floor, a pool bottom, the submerged part of a pier), right after the
  cloud shadows and before the sun shafts, the flare and the underwater composite (after TAA and the exposure histogram,
  so the web never enters the TAA history or pumps the exposure). One fullscreen pass (`kCausticsFS`, its own shader and
  its own `CausticsU` block; the lit shaders are not touched) rebuilds each pixel's position from the half-resolution
  opaque prepass, follows the sun up to the wave-free plane (`level_y`, inside the drawn water square) and reads a
  procedural web there: 2 (Medium) or 3 (High) octaves of a cellular "ridged" noise on an integer hash, its feature points
  moving on closed loops driven by a clock that is wrapped as whole ticks before it becomes a float, so the web repeats
  exactly every 4 s / `speed` and never drifts with a long session. The light added is sun colour * intensity *
  strength * the sun's share of the light * pattern * exp(-absorption * (sun path + view path in water)) * a depth falloff
  (in over the first 0.25 m, out toward `max_depth`) * the sun-elevation fade, scaled by the exp² fog's surface share.
  The web is anchored to the world (the camera's world position is folded into the pattern's period on the CPU in f64),
  so it stays on the floor as the camera moves and a floating-origin rebase leaves it in place. It draws with the camera
  above the water and under it (the underwater composite follows). New `mce/render/caustics.h` holds the CPU twin
  (`caustics_phase`, `caustics_offset`, `caustics_pattern`, `caustics_term`, the fades, `caustics_gate`); the shader is
  tested against it. Fills `Forward3DStats::caustics` / `caustics_strength`; new golden `caustics`. The setting off, the
  Low tier, `r.caustics 0`, no water, a sun under the fade band, below the horizon or NaN, no sun light, an orthographic
  view and render-to-texture views record nothing, and the pass only adds light, so a frame without it is unchanged bit
  for bit (every existing golden is unchanged). Limits: the pattern is procedural (it does not follow the waves; calm
  water still shows it), the plane ignores the swell, the light is not tinted by the water's colour nor darkened by cloud
  shadows, the pass needs the prepass pipeline (SSAO, SSR, contact shadows or the edge outline on at startup), and a
  silhouette pixel takes its half-resolution block's sample.
