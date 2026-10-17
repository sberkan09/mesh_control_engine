- Material height maps, M-VISUAL3 step 2a (SBA-199): `.mcmat` takes `textures.height`, `height_scale` (0..0.5) and
  `parallax_steps` (`[min, max]`, 4..64), cooked into `MaterialExt` (importer version 2). The kit packs the normal and
  height maps into one 2-layer array under a new texture key (`MaterialOverride::parallax`, `parallax_normal`), and a
  height map that does not match the normal map logs one warning and draws without parallax. The shader that marches
  it is the next step; until then the keys cook and extract but do not change a frame.
