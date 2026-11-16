- Ship block-compressed textures, M-CONTENT step 1b-iv (GH-388): `mce cook --textures rgba8|bc|astc` and `mce ship --textures
  rgba8|bc` (desktop and script projects re-cook into `<project>/assets.mce`, combinable with `--max-texture-size`),
  `scripts/ship.py --textures rgba8|bc` (android and ios always cook ASTC and refuse `bc`; the web stays `rgba8` and
  refuses it, as `mce ship --target web --textures bc` does), and MCP `cook_run {"textures": "rgba8"|"bc"}`. A size gate
  on `samples/05_ashen_court` holds the BC cook at 0.27 of the RGBA8 texture bytes with both normal maps in BC5. Docs:
  user guide chapters 5, 15 and 16.
