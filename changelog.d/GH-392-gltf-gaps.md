- glTF gaps, M-IMPORT3D step 1 (GH-392): the glTF importer reads sparse accessors (attributes, indices, animation,
  inverse bind, morph deltas), `KHR_mesh_quantization` (BYTE / SHORT positions, normals, UVs), `EXT_meshopt_compression`
  (meshoptimizer decoders, filters included; the fallback buffer is ignored) and every `JOINTS_n` / `WEIGHTS_n` set (the
  4 largest influences per vertex, renormalized, with a warning counting vertices that lost more than 1 % of their
  weight). The default morph weights are baked into POSITION / NORMAL so the rest pose matches other viewers (the
  targets are still dropped, the warning now names their count). `KHR_draco_mesh_compression` is refused when required
  (the error says to re-export with `gltfpack -c` or `gltf-transform meshopt`) and its fallback read when only used.
  The clearcoat, sheen, anisotropy, transmission and ior material extensions join the supported list (no more
  "ignored" warning). `COLOR_0`, `TEXCOORD_1+`, cameras and `KHR_lights_punctual` each warn once per file with the first
  JSON pointer. The file is treated as untrusted: accessor, sparse, bufferView and compressed-stream ranges are checked
  without overflow, and sizes are capped (16M elements per accessor, 256 MiB per decoded view, 32M vertices per scene)
  with an error naming the pointer. Importer version 4. `mce_meshopt` now also compiles the vertex / index codecs and
  filters. Tests: `tests/asset/test_cook_gltf_ext.cpp`.
