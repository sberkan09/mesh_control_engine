- Texture size cap, M-CONTENT step 1a (GitHub #383): ordinary PNG/JPEG textures take a sidecar `"max_size"` (a power of two; the image is
  box-halved before its mips), and `mce_cook`, `mce cook`, `mce ship` and MCP `cook_run` take a project-wide `--max-texture-size N`
  (`max_texture_size`); the smaller of the two applies, and the default is no limit (existing packs cook to the same bytes). Every cook
  now ends with a size report: bytes by type, the five largest assets and how many textures the cap cut (`sizes` in `cook_run`).
