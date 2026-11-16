- Strikes with a place, M-LIGHTNINGBOLT step 11a: the optional `LightningBolt` component (ranges, `ground_y`, bolt
  authoring, the last strike saved) gives each `kit_lightning` strike a deterministic ground position near the primary
  camera (own hash stream `counter * 4 + 2/+3`, so strike times and delays of existing scenes do not move).
  `LightningStruck` gains `has_pos`, `distance_m` and `pos`; `ThunderDelay` uses `distance_m` for delay and gain when
  `has_pos` is set and is unchanged otherwise. A scene without a `LightningBolt` is bit-identical. Guide 9.19g.
