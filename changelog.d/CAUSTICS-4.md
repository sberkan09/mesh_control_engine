- Caustics look, M-CAUSTICS after step 52d: the web now follows how squarely a surface faces the sun (the prepass's
  normal, relative to a flat floor: a floor gets all of it, a steep wall, a face turned away from the sun or lit edge-on
  none), so steep submerged walls no longer show stretched white streaks; the light is added in the surface's own colour
  (`caustics_composite`: the pixel's chroma times the light's luma), so on sand it reads as warm sunlight rather than
  white lines; and its lines have a softer falloff (1 - smoothstep(0, 0.16, F2 - F1), no longer squared). The CPU twin has
  `caustics_facing` and `caustics_composite`, and `caustics_term` takes the surface normal; golden `caustics`
  regenerated. The `tour_caustics` sample uses strength 1.2 (the default stays 1).
