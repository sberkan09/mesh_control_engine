- `StaticMerge3D`, M-VISUAL3 step 5b (SBA-199): put it on an entity and the rigid `MeshRef` children (and their
  children) merge at runtime into one mesh per material per cell of `cell_size` metres (default 32), drawn in their
  place, so a modular kit of different pieces sharing a colormap costs draws = materials x cells per pass instead of
  one per piece. Animated, blended, transmissive, parallax and `interpolate` children stay separate; moving a child
  in the editor, or bumping `version`, re-merges and retires the old mesh; removing or disabling the component restores
  the per-child draws. New `render::build_merged_mesh(Span<const MergeSource>, Array<byte>&)` (`render/static_merge.h`:
  world placement, normals by the inverse transpose, re-quantization over the merged bounds, one submesh per merge key,
  per-level LOD union, 32-bit indices past 65536 vertices), `kit/static_merge3d.h`, `Kit3DExtract::set_static_merge`
  (the editor viewport turns it off so selection and picking stay per entity) and `Kit3DStats::merged_groups`,
  `merged_children`, `merged_cells`. Nothing changes for scenes without the component (every existing golden is
  byte-identical). New golden `static_merge_kit`; chapter 8 says when to use it against automatic instancing and
  `Foliage3D`.
  `perf_run`, `.mcperf` captures and `perf_compare` inputs carry `merged_groups` and `merged_children`.
