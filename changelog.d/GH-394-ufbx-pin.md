- ufbx pinned for FBX import, M-IMPORT3D step 3a (GH-394): ufbx v0.23.1 (MIT or Unlicense) by commit, cook-only as
  `mce_ufbx` (`-ffp-contract=off` / `/fp:precise`, its OBJ and geometry-cache readers compiled out), confined by the
  "ufbx wall" and "ufbx link wall"; ADR-0047 records why ufbx over Assimp and the Autodesk FBX SDK, the memory limits
  every FBX load sets, and the libm paths that keep FBX cooks per-host for now. `test_ufbx_smoke` loads an in-test
  ASCII FBX cube and survives truncated, mutated and huge-claim files. No `.fbx` importer yet (step 3b).
