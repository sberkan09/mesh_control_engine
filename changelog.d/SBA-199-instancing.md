- Automatic instancing, M-VISUAL3 step 5a (SBA-199): copies of one rigid submesh with the same material and tint
  (`MeshRef`s and `StaticInstances3D` copies alike) now draw as one instanced draw per pass in the main pass, the SSAO
  and velocity prepasses, every sun cascade and every local-light shadow tile, with the same pixels. `Scene3D` gains
  `DrawRun`, `InstancingSettings` / `set_instancing`, `opaque_runs()` and `caster_runs()`, and the counters
  `runs_instanced`, `draws_instanced` and `not_instanced_{blended,terrain,skinned,moving,single,disabled}`;
  `Forward3DStats` gains `runs_instanced`, `draws_instanced`, `shadow_runs_instanced`, `shadow_draws_instanced`,
  `instancing_overflow` and `instancing_bytes`; `Forward3DDesc` gains `instancing`, `instancing_budget` and
  `geometry_ring_bytes` (the frame's instancing uploads stay under a quarter of the geometry ring, the main pass
  reserved first), `InstancingSettings::max_count` (`kMaxInstancesPerRun`) splits longer runs, and `-r.instancing 0`
  turns it off. On by default (every existing golden is byte-identical with it on); the showcase's main-pass draws fall
  by about a third and its shadow draws by about 30 %. New golden `instanced_props`.
