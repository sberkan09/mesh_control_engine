- Caustics Luau, sample and docs, M-CAUSTICS step 52d: the Luau `weather.set_caustics{strength=, scale=, speed=,
  absorption=, max_depth=}` with the getters `weather.caustics_strength()`, `caustics_scale()`, `caustics_speed()`,
  `caustics_absorption()` and `caustics_max_depth()` over the first `Caustics3D` (named errors, each value clamped to the
  field's range, nothing written on an error, `enabled` not settable), typed in the Luau definitions; the
  `tour_caustics` scene in `kit3d_tour` (a shallow pool with a sand floor and three rocks standing half out of the water
  under a high sun; `gen_caustics.py`, headless test and High-tier GPU capture; `-r.caustics 0` draws it without the
  web), and the user guide (the caustics item and the `PostFx` row in 8.6, the scripting table in 27.3, the
  `kit3d_tour` row in 18).
