- Import summary, M-IMPORT3D step 4b (GH-398): after a cook, `mce_cook` prints one `mesh assets/...: N tris, N submeshes, N
  materials, N joints, N clips, N LODs` line per 3D model, and MCP `cook_run` returns the same fields in `meshes`
  (read from the cooked file, so a cached model reports the same numbers; `warnings` counts the current run's only).
  `samples/import_demo` gains a generated OBJ + MTL, an ASCII FBX and a glTF with a light and a camera that
  `mce import gltf` turns into a prefab, all checked by `import_demo_reimport`. User guide 8.4, 5.3 and 14 cover it.
