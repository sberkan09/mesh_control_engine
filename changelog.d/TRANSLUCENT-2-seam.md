- Translucency seam, M-TRANSLUCENT step 41a (TRANSLUCENT-2): `MaterialFlags` reserves bits 8..15 (translucency) and 16..23 (wrap) of
  `MaterialChunk::flags` as quantized 0..1 factors, with the accessors in `mce/asset/material_translucency.h`;
  `Forward3DDesc::translucency` and `Forward3DStats::translucent_draws` are added. Data only: nothing reads them yet, every existing
  material reads 0 (off) and no cooked format changes.
