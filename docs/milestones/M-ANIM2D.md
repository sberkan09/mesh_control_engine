# M-ANIM2D — 2D skeletal animation (milestone record)

**Status:** active (steps 1 and 2 done; step 3 next).
**Why:** 2D characters animate only by sprite frames today; cut-out rigs (bones, blended clips, skinned meshes, IK) are what Godot's Skeleton2D, Unity's 2D Animation and Spine give 2D games. Added 2026-10-16 under the user's standing instruction to extend the roadmap when no milestone is free.
**Plan:** ROADMAP § M-ANIM2D.
**Measured on:** the Linux CI container.

## Steps

| Step | Linear | State |
|---|---|---|
| 1. `.mcskel2d` assets, `Skeleton2D` world pose, slots drawn through the sprite extraction | SBA-140 | done |
| 2. Bone clips and crossfade, driven by the `.mcanim` state graph | SBA-141 | done |
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

## Step 2: bone clips, crossfade and the state graph

| Item | Where | Evidence |
|---|---|---|
| `.mcskel2d` `"animations"`: named clips of per-bone `rotate` / `translate` / `scale` keys (offsets from the rest pose: degrees and metres add, scale multiplies), `"duration"` (default: the last key or event), `"loop"` (default true) and `"events"`. Cooked into the same `CHNK_SKEL2D` as minor 1 (`SKELETON2D_MINOR_ANIMATIONS`): the header's four reserved words became the animation, timeline, key and event counts, so a minor-0 chunk reads unchanged and a skeleton without animations cooks byte for byte as before | `src/engine/asset/cook/src/skeleton2d_importer.cpp`, `mce/asset/formats.h` | `test_kit` "skeleton2d anim: animations cook into CHNK_SKEL2D minor 1" |
| Curves: `"linear"`, `"stepped"`, or a cubic bezier `[cx1, cy1, cx2, cy2]` in the normalized key interval (Spine's convention), solved by a fixed 24-step bisection (no tolerance exit) | `asset::skeleton2d_sample`, `asset::skeleton2d_bezier` in `src/engine/asset/src/formats.cpp` | `test_kit` "skeleton2d anim: linear, stepped and bezier keys sample exactly" (a linear midpoint, a step, x(s) = s giving 0.875, CSS ease at 0.5 = 0.8024034) |
| `Skeleton2DView::read` refuses keys out of order or at the same time, negative or non-finite times and values, timelines on a missing bone, unknown channels and curves, a bezier cx outside 0..1, empty or overrunning key, timeline and event ranges, bad names, unknown flags and too many animations; a minor-0 payload reads | `src/engine/asset/src/formats.cpp` | `test_kit` "skeleton2d anim: the reader rejects bad animation data and reads a minor-0 payload" |
| Cook errors name the file position, the animation and the bone: unsorted keys, an unknown bone, a bad curve, a bezier cx out of range, a key without `t`, an empty key list, duplicate animation names, events out of order, a long event name, a negative duration, an unknown key | same | `test_kit` "skeleton2d anim: cook errors name the animation and the bone" |
| `Skeleton2DAnimator` (clip, speed, fade, time, loop, finished, restart; previous clip, its time and the blend weight as runtime fields; graph, params, state). `"kit_skeleton2d_animators"` runs just before `"kit_skeleton2d"` in `FixedUpdate_Post` and writes the blended channels into `Skeleton2D.pose`. Only channels the sampled clips key are written; others keep gameplay's writes | `mce/kit/skeleton2d.h`, `src/engine/kit/src/skeleton2d_anim.cpp` | `test_kit` "skeleton2d anim: a looping clip wraps and fires each event once per crossing", "a cross-fade blends by weight and a one-shot holds its end" (rotation, translation and scale at blend 0.5: 1.5 for a 2 against the rest), "the animators run before the skeletons in FixedUpdate_Post" |
| The `.mcanim` graph drives 2D clips through the 3D Animator's state machine step, moved unchanged into a template shared by both (`src/engine/kit/src/anim_graph_step.h`): parameter transitions, one-shot finish, fades, 1D and 2D blend states, state events. Layers, root motion and IK stay 3D only | `src/engine/kit/src/anim_graph_step.h`, `src/engine/kit/src/anim3d.cpp` | `test_kit` "skeleton2d anim: an AnimGraph drives 2D clips on a parameter and on a one-shot's end"; all `anim3d:` and `kit3d:` cases and the `scene_demo` / `07_gameplay3d` hash tests unchanged |
| Events: clip and state events raise `AnimEvent3D{entity, StringId}` once per crossing, again across a loop wrap, and at a one-shot's end | `skeleton2d_anim.cpp` | "a looping clip wraps and fires each event once per crossing" (the read order start, tick, start, tick, start) |
| Determinism and saves: every animator field is `Save`; two runs hash the same; a save in the middle of a cross-fade resumes with the same pose and the same hash on the next tick | `test_kit` | "skeleton2d anim: two runs hash the same, and a save mid-fade resumes identically" |
| MCP `asset_schema` / template describe `"animations"`; the three component registries list `Skeleton2DAnimator` | `src/editor/mcp/src/asset_tools.cpp`, the `engine_components.cpp` files | ctest `mcp_transcript` (golden regenerated: one more component); `scripts/ci/capabilities.py --check` |

Deferred from step 2: slot color and alpha keys and attachment swaps move to step 3, where mesh
attachments add per-slot runtime state. Rotation offsets blend linearly, not by the shortest arc,
so a cross-fade between rotations more than half a turn apart turns the long way.

