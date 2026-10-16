# M-ANIM2D — 2D skeletal animation (milestone record)

**Status:** active (step 1 done; step 2 next).
**Why:** 2D characters animate only by sprite frames today; cut-out rigs (bones, blended clips, skinned meshes, IK) are what Godot's Skeleton2D, Unity's 2D Animation and Spine give 2D games. Added 2026-10-16 under the user's standing instruction to extend the roadmap when no milestone is free.
**Plan:** ROADMAP § M-ANIM2D.
**Measured on:** the Linux CI container.

## Steps

| Step | Linear | State |
|---|---|---|
| 1. `.mcskel2d` assets, `Skeleton2D` world pose, slots drawn through the sprite extraction | SBA-140 | done |
| 2. Bone clips and crossfade, driven by the `.mcanim` state graph | SBA-141 | — |
| 3. Mesh attachments and CPU skinning, deform keys | SBA-142 | — |
| 4. Two-bone and CCD IK as data | SBA-143 | — |
| 5. Editor skeleton panel and MCP `skel2d_*` tools | SBA-144 | — |
| 6. Demo, user guide, gates | SBA-145 | — |
| Later: Spine / DragonBones JSON import | SBA-146 | not scheduled |

## Step 1 — skeletons, the world pose and the slots

| Item | Where | Evidence |
|---|---|---|
| `.mcskel2d` cooks to `CHNK_SKEL2D` (`ChunkType::Skeleton2D`, appended; `SKELETON2D_VERSION` 1): bones with rest poses (degrees in the text, radians cooked), slots in draw order, a string pool, reserved fields and a slot `kind` for later steps. Slot textures are dependencies | `src/engine/asset/cook/src/skeleton2d_importer.cpp`, `mce/asset/formats.h` | `test_kit` "skeleton2d: .mcskel2d cooks to CHNK_SKEL2D and a scene poses it" |
| Cook errors name the file position and the bone or slot: a parent listed after its child ("list parents before their children"), an unknown parent or slot bone, duplicate names, a bad array, an unknown key, no bones, more than 64 bones, a texture that is not in the project | same | `test_kit` "skeleton2d: cook errors name the bone and the file position" |
| `Skeleton2DView::read` refuses parents at or after their child, a first bone with a parent, slot bones out of range, string offsets out of range, a pool without its NUL, non-finite numbers, unknown slot kinds, more than 64 bones and truncated payloads | `src/engine/asset/src/formats.cpp` | `test_kit` "skeleton2d: the reader rejects bad payloads" |
| Pose math: world = parent world x local translate * rotate * scale, one pass in parent-first order; offsets add (x, y, rotation) and multiply (scale) | `mce/kit/skeleton2d.h` (`skeleton2d_world_pose`, `skeleton2d_pose_offsets`, `bone2d_offset`) | `test_kit` "skeleton2d: world pose of a parent chain, inherited rotation and non-uniform scale" (exact chain sums, a 180 degree inherited turn, a stretched parent's turned child with axes (0, 1) and (-2, 0)) |
| `Skeleton2D` component: skeleton, tint, layer, order, flips, visible, and 64 per-bone `pose` offsets (`Save`) beside the engine-written `world`. `"kit_skeleton2d"` (`FixedUpdate_Post`, in `install_stock`) poses it; `Bone2DFollow` moves an entity onto a named bone, in world space or in the skeleton's space under `ChildOf` | `src/engine/kit/src/skeleton2d.cpp`, `src/engine/kit/src/stock.cpp` | `test_kit` "...a scene poses it" (the sword lands at hero + hand x offset, z kept), "a follower parented to the skeleton gets the bone in skeleton space", "install_stock installs the skeleton system" |
| Slots drawn by `SpriteExtract` in slot order at entity * flip * bone * slot, with rect, size, pivot, slot tint x component tint, layer and order | `skeleton2d_quads`, `src/engine/kit/src/sprite_extract.cpp` | `test_kit` "skeleton2d: slots become quads in slot order at entity * flip * bone * slot" (positions, rotation, sizes, tints, a mirrored skeleton, a pose offset, a hidden skeleton) |
| Determinism and saves: two runs hash the same every tick; a save restores the pose and the next tick hashes the same; the scene text keeps `pose` and drops `world` | `test_kit` | "two runs hash the same every tick, and a save restores the pose", "the scene text keeps the per-bone offsets and drops the derived pose" |
| MCP `asset_create` / `asset_schema` know `Skeleton2D` (`.mcskel2d`), its template cooks; the asset panel, `asset_list` and the three component registries know it | `src/editor/mcp/src/asset_tools.cpp`, the `engine_components.cpp` files | `test_mcp` "asset_create / asset_write / asset_read author every JSON source type"; ctest `mcp_transcript` (golden regenerated); `scripts/ci/capabilities.py --check` |

Limits found in step 1: within one sorting layer the 2D batch orders quads by texture before
`order`, so slots keep exact slot order only on one atlas texture. A sprite quad cannot shear, so a
slot under a rotated child of a non-uniformly scaled bone loses the shear. A `.mcscene` lists all
64 `pose` entries (the scene codec reads fixed arrays whole). Slots naming a sprite-sheet region are
not built; a slot takes the sheet's texture and a `rect`.

