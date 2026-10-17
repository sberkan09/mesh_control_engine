- Player: the pack's convar-defaults chunk (`.mcdefaults`) is applied at start-up on every platform, before
  `defaults.cfg`; a project with two `.mcdefaults` files is a cook error naming both; a pack without the chunk starts
  as before; a mod's pack is data and its defaults are ignored. Undertow ships `ui.reference_height = 720`.
  (M-WORKFLOW 7, PR 4b-2, #382)
