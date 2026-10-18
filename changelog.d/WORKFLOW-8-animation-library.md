- Assets: a glTF sidecar's `"animations_from": ["animations/lib.glb"]` binds the clips of a shared animation library
  to the mesh's nodes by bone name at cook, so one library serves every character with the same skeleton without
  merging it into each glTF; the library is a dependency, a clip of the mesh wins a name clash, bones the mesh lacks
  are skipped with a count, and a library that matches no bone is an error. Kit3D now says once, when a scene has
  MeshRef entities and draws none, how many are hidden, have no mesh set, have no Transform, are culled or whose mesh
  is not loaded. (M-WORKFLOW 8, #409)
