- Lights and cameras of 3D models, M-IMPORT3D step 4a (GH-397): `mce import gltf <model>` and `mce import fbx <model>`
  (and MCP `asset_import` with `tool` `gltf` / `fbx`) write `<name>.mcprefab` beside the model: a root entity with a
  `MeshRef` to it and a child per `PointLight3D`, `SpotLight3D` or `Camera3DComponent` at the node's world transform in
  the engine's frame (the first camera primary). glTF (`KHR_lights_punctual`, cameras, `matrix` or TRS nodes, GLB) is
  parsed in Python with bounded sizes, node count and depth; FBX is read by the cook's own reader through the new
  `mce_cook --dump-fbx-scene <file>` (JSON of lights, cameras and world transforms), so there is no second FBX parser.
  Directional lights get no entity: a warning names `Environment3D.sun_direction`, the direction found and a suggested
  `sun_intensity`. Intensity is a fixed documented mapping (glTF candela `/ (683 pi)`, FBX `Intensity / 100`, user guide
  8.4). Same rules as the 2D converters: same bytes twice, kept GUIDs, named warnings, `--werror` writes nothing.
  Test `import_3d` and an `asset_import` MCP case; the MCP transcript golden changes in the `asset_import` schema only
  (the integration runner regenerates it).
