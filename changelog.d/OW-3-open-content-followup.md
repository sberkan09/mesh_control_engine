- Open-licence content line follow-up, M-OPENWORLD D2/D3: `mce content add` packs a PBR set's roughness and occlusion maps into the
  engine's `metallic_roughness` texture (the ambientCG ledger entries now pin the 1K-PNG archives); `mce retarget --map-only`
  retargets skinless node-animated rigs and warns when the output is away from the character's textures; the slow test
  `import_content_real` fetches real packs by ledger hash and cooks them with `mce_cook`.
