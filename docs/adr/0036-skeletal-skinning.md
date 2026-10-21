# ADR-0036: Optional skeletal skinning

**Status:** Accepted (2026-10-10). **Milestone:** M-SKIN (`docs/milestones/M-SKIN.md`). **Requirements:**
FR-R3D-060 (built, stays Should; REQUIREMENTS §8 Open Question 3 update, 2026-10-10). **Supersedes:** ADR-0022 §7
("skins ... ignored with a warning") and the "No skinning at MVP" line of ADR-0023.

**Context.** ADR-0022 locked glTF import to static meshes plus node-transform clips, and ADR-0023 drew every mesh
rigidly from a node pose. On 2026-10-10 the user asked for skinned animation without making a skeleton mandatory:
a developer chooses skinned or skeleton-free per asset, and even per instance. glTF skins name ordinary nodes as
their joints, so the existing node pose (`pose_nodes`, `pose_nodes_blend`, `AnimationPlayer` cross-fades) already
animates a skeleton; only the last step was missing.

## Decisions

1. **Skins ride in `CHNK_MESH`, minor 1.** Like ADR-0022's clips, a skin only binds the nodes of the file it came
   from, so it is a section of the mesh chunk, not a new chunk: `MeshFlags::Skinned`, `MeshSkinHeader`, `MeshSkin`,
   `MeshJoint` and one `MeshSkinVertex` per vertex (`mce/asset/formats.h`, `MESH_MINOR_SKIN = 1`). A static mesh
   still cooks to the minor-0 layout byte for byte. `MeshView::read` validates the section like the rest (joint
   index past the skin, joint node past the tree, missing skin, stray offset).
2. **glTF importer v3 imports skins.** Joints are remapped to imported nodes; inverse bind matrices default to
   identity; `JOINTS_0` (u8/u16) and `WEIGHTS_0` (float or normalized) quantize to 4 × unorm8 summing to 255; at
   most 256 joints per skin; each joint stores the bind-space box of the vertices it moves. The skinned node's own
   transform is ignored, as glTF specifies. Morph targets, cameras and lights are still ignored with a warning.
3. **Skinning is optional at two levels.** Sidecar `{ "skinning": false }` cooks a skinned file as a static mesh;
   `MeshInstance::skinning = false` draws one instance rigidly in its bind pose.
4. **Palette evaluation on the CPU.** `skin_palette` computes `palette[j] = pose[joint node] * inverse_bind[j]`;
   `skin_bounds` unions the joints' bind boxes under the palette (conservative: a blended vertex lies in the convex
   hull of its joints' moved positions). `Scene3D` concatenates every skinned instance's palette
   (`Scene3D::joint_palette()`) and each `Draw` carries its first joint.
5. **One RHI binding bank and one vertex stream.** The palette is a storage buffer at `rhi::bank::kJointPalette`
   (7, `mce/rhi/types.h`), uploaded once per frame with `set_storage_data` before the shadow pass. Because the RHI
   binds one vertex buffer per draw, a skinned mesh uploads one interleaved 24-byte `SkinnedMeshVertex`, and all its
   submeshes draw through the four skinned pipelines in `Forward3D` (forward and shadow); rigid submeshes take the
   identity skin.

## Consequences

- Rigid node hierarchies are unchanged and remain the default; the joint palette is part of the headless
  determinism hash of `04_cubes`.
- Not built: morph targets, more than 4 influences (`JOINTS_1`), dual-quaternion skinning, GPU palette compute, an
  editor skeleton preview and an ECS animation component (3D animation stays host-driven).

## Tests

- `test_cook`: skins import joints, inverse binds, 4 unorm8 influences and per-joint bounds; skinning off per asset;
  bad skins fail with the JSON pointer.
- `test_render`: the skin section round-trips and corrupt sections are rejected; the palette is the identity at rest
  and follows a bent joint; one palette per skinned instance, with opt-out; playback events.
- `test_render_gl`: a skinned bone bends with its joint and skinning off draws the bind pose, with zero GL
  validation errors.
