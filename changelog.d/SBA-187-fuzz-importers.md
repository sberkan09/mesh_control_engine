- Fuzzing covers the importers, M-FUZZ step 3b (SBA-187): `test_fuzz_importers` mutates committed sources of 25
  extensions (valid JSON with one value swapped as well as raw bytes) and calls each importer; no crash. It found
  that the glTF importer asserts on a wrongly typed accessor value and that a damaged `.ttf` crashes the font
  importer's atlas bake (stb_truetype); the font case is skipped unless `MCE_FUZZ_FONT=1`.
