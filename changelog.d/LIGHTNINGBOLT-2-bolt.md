- A visible lightning bolt, M-LIGHTNINGBOLT step 11b: `build_bolt` / `build_bolt_fork` (pure, deterministic midpoint
  displacement from the strike point and `Lightning.counter`, at most 129 points, optional side fork) and a
  `Kit3DExtract` draw as emissive box segments of one unit box mesh (no shader or pass), brightest on the first flash
  tick with a hashed re-strike on the second, an optional blue-white `PointLight` when `light_intensity` > 0. Nothing
  is drawn outside a flash, outside a Storm, with `enabled` 0 or on the Low tier; the extract writes no component. A
  scene without a `LightningBolt` is bit-identical. Guide 9.19g.
