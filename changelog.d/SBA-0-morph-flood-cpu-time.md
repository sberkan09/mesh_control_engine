- Test hardening: the morph-bake flood test in `test_cook_gltf_ext` measures CPU time (limit 5 s) instead of wall-clock (1 s), so a loaded
  `local_ci --task` run no longer fails it, while a refusal that reads the deltas first (tens of CPU seconds) is still caught.
