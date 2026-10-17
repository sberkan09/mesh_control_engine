#pragma once
// StaticMerge3D (M-VISUAL3 5b): merges the rigid MeshRef children (and their children) of this entity into one mesh
// per material per cell of `cell_size` metres at runtime, drawn in place of the children. Modular kits (Kenney's
// pieces share one colormap) are many different small meshes with one material; this turns them into a few draws.
//
// A child qualifies when it is visible, rigid (no skin and no posed nodes), has no Animator, interpolate 0 and an
// opaque or masked material, its mesh is resident and its asset is mounted (the merge reads the cooked vertices);
// the others draw as before, one by one. A child belongs to the cell its mesh's bounds centre falls in, and cells
// split by shadow casting, so a merged cell is one MeshInstance per cast_shadows value (almost always one).
//
// The merge happens on the first extract and again when the children's (entity, mesh, material, transform, tint)
// hash or `version` changes: moving a child in the editor re-merges by itself, a game that moves children at
// runtime bumps `version` (or, better, leaves them out of the merge: an entity that moves is not static). The
// merged meshes live in the MeshStore under keys from MeshStore::allocate_key() and are released when the
// children change, the component is removed or disabled, or the extract shuts down.
//
// The editor viewport turns merging off (Kit3DExtract::set_static_merge(false)) so selection, gizmos and picking
// keep per-entity draws. Use StaticInstances3D or the automatic instancing (M-VISUAL3 5a) for many copies of one
// mesh and Foliage3D / TerrainFoliage3D for scattered vegetation; this is for many different meshes.
//
// Layout: padding-free (World::state_hash hashes component bytes).

#include "mce/refl/macros.h"

namespace mce::kit {

struct StaticMerge3D {
    f32 cell_size = 32.0f;  // metres; bounds vertex precision (snorm16 over a cell: 0.5 mm at 32 m) and culling
    u32 cast_shadows = 1;   // 0: the merged meshes cast none (the children's own flags are ANDed otherwise)
    u32 enabled = 1;
    u32 version = 0;        // bump after moving children at runtime (edits in the editor re-merge by themselves)
};
static_assert(sizeof(StaticMerge3D) == 16);
MCE_COMPONENT(StaticMerge3D, (cell_size, Range{4, 512}), cast_shadows, enabled, (version, Help{"bump to re-merge"}))

}  // namespace mce::kit
