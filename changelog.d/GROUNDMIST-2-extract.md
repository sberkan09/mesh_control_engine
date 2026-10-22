- Morning mist drawn, M-GROUNDMIST step 13b: `Kit3DExtract` feeds the `GroundMist` level into its own copy of the
  volumetric fog (density, height falloff, base height, albedo) or, when volumetric fog is off or the tier drops it, into
  the exp2 fog (density, colour), one path per frame. `Environment3D` is never written and a scene without a
  `GroundMist`, with `enabled` 0 or at level 0 renders as before. Guide 9.19i.
