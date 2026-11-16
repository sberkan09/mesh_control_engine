- M-VISUAL3 demo and close (SBA-199): `samples/06_showcase` gains the profiles `visual3` and `visual3off` and a fourth shot,
  "meadow": a parallax cobble floor, the filmic lens, a cooked HDR dusk sky, a yard of 40 crates drawn by automatic
  instancing, a 25-piece modular wall merged into one mesh (`render::build_merged_mesh`, what `StaticMerge3D` runs) and a
  windy meadow of two kinds with colour variation, with the main pass's draw counts printed by `-sheet`. The sample's device
  uses the player's 4 MiB uniform ring. `StaticMerge3D` leaves `sample_coverage.allow`; the milestone record
  `docs/milestones/M-VISUAL3.md` is closed with the per-step table, the known limits and the sheet.
