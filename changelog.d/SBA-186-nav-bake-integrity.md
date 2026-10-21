- Nav mesh bakes carry an integrity hash and their layers are checked, M-FUZZ step 2b (SBA-186): `test_fuzz` found
  that a bake with damaged layer bytes crashed Detour's contour builder (division by zero). `.mcnav` version 2
  ends with an FNV-1a 64 of the bytes before it, and `navmesh_blob_valid` (also behind `NavMesh3D::load`) decodes
  every tile layer and refuses a bad header, tile coordinates, area id or a connection that leaves the grid, is not
  mutual or joins a non-walkable cell, even when the hash was made right again; the importer refuses a hash
  mismatch, and a version-1 file asks for a re-bake (the sample's `nav_13.mcnav` was upgraded).
