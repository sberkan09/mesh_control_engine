- Draw-call report, M-CONTENT step 2 (GH-389): `perf_run` returns `draw_causes`, the 3D scene draws that were not part of
  an instanced run counted by cause (`material`, `unique_mesh`, `skinned`, `terrain`, `moving`, `blended`,
  `instancing_off`, `budget`, `other`) with a one-line hint for each, and the new `mce perf [<project | file.mcperf>]
  [--json]` prints them as a table. `Forward3DStats::draw_causes` and `Scene3D::Stats::not_instanced_unique_mesh` /
  `not_instanced_short` carry the counts; `.mcperf` files name the causes in their header and keep the counts in each
  frame's `counters.draw_causes` (version 1, older files read as zero). Counts only: nothing time-dependent.
