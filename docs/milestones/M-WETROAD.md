# M-WETROAD — wet roads

Owner: rutin 2 (M-VISUAL). Follow-up of [M-PAINTWEAR](M-PAINTWEAR.md) step 2 (the worn paint is alpha-masked; wet paint should lose its sheen).

**Problem.** `samples/09_openworld` is always dry: asphalt, paint and paving have one fixed roughness, so rain or a wet night cannot change how the street looks.

**Plan.**
1. A `wetness` scene parameter (0..1) that lowers roughness and darkens the base colour of road and paving materials in the forward shader. Needs the coordinator: it touches the material path.
2. Per-material response: a roughness map for worn paint (rougher where chipped, so it stays dull when wet) and puddles from a low-frequency mask on the asphalt texture.
3. Screen-space reflections of the street lights on the wet surface (reuses the M-REFLECT2 SSR).

**Check.** Same junction frame dry and at wetness 1, side by side.
