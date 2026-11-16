- Mesh back end seam, M-IMPORT3D step 0 (GH-391): the glTF importer's format-neutral mesh back end (meshopt pass,
  quantization, LOD generation, skin tables, `CHNK_MESH` fill) moves out of `gltf_importer.cpp` into the cook-private
  `mesh_build.h` / `mesh_build.cpp`, so the OBJ and FBX importers can fill the same primitive struct. No behaviour
  change: `tests/asset/test_cook_mesh_golden.cpp` pins the `CHNK_MESH` hash of every sample glTF and of the cook
  tests' fixtures, recorded on the base commit.
