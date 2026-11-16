- FBX importer, M-IMPORT3D step 3b (GH-395): `.fbx` files (binary or ASCII) under `assets/` cook to the mesh asset on
  ufbx (host builds). Meshes with normals and UV set 0, inline PBR materials, the node tree, skins (4 largest influences,
  256 joints) and animation stacks as clips sampled at the sidecar `sample_rate` (default 30; base layer only) are
  imported in the engine's frame (+Y up, right-handed, metres): a Z-up centimetre file and Blender's 0.01 / 100 scale
  pair come out like the glTF cube; sidecar `scale`, `max_triangles` and `lod_levels` work as for glTF. Embedded
  textures are an error ("export with external textures"); hidden meshes, blend shapes, extra animation layers, lights
  and cameras warn once per file. The file is untrusted: ufbx runs under memory and depth limits and every count is
  capped with an error naming the cap. FBX cooks are byte-identical per host, not across hosts (ADR-0047). The editor
  treats `.fbx` as media. Tests: `tests/asset/test_cook_fbx.cpp`.
