- `compress: auto` under `--textures bc` picks BC5 for a linear texture when at least 85% of its texels are unit-length
  normals (was 99%). Measured on 14 Poly Haven normal maps, 99% picked BC5 for 4 and 85% picks it for 13; colour maps
  pass under 1%. The texture importer's version is bumped, so `bc` cooks redo their textures once.
