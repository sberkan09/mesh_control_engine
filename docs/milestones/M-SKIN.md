# M-SKIN — Optional skinned animation (milestone record)

**Status:** merged into `dev_claude`. FR-R3D-060 is built and stays **Should**: nothing requires a skeleton.
**Why:** the user asked for skinning support without making a skeleton mandatory for the main character; developers
choose skinned or skeleton-free (REQUIREMENTS §8 Open Question 3 update, 2026-10-10).
**Measured on:** the Linux CI container (Mesa llvmpipe GL 4.5, Xvfb).

## The choice a developer has

| Character | How |
|---|---|
| static mesh or rigid node hierarchy (the default, unchanged) | export without a skin; node clips animate it as before |
| skinned mesh | export the glTF with its skin; the clips animate the joints, which are ordinary nodes |
| a skinned file used without its skeleton | sidecar `{ "skinning": false }`: cooked as a static mesh |
| one instance without its skeleton | `MeshInstance::skinning = false`: drawn rigidly in its bind pose |

## What shipped

| Item | Where | Evidence |
|---|---|---|
| glTF skin import: joints remapped to imported nodes, inverse bind matrices (identity when absent), `JOINTS_0` (u8/u16) + `WEIGHTS_0` (float or normalized) quantized to 4 × unorm8 summing to 255, ≤ 256 joints per skin, per-joint bind-space bounds | `gltf_importer.cpp` (importer v3) | `test_cook`: "skins import joints, inverse binds, 4 unorm8 influences and per-joint bind bounds", "skinning is optional per asset; bad skins fail with the JSON pointer" |
| `CHNK_MESH` skin section | `formats.h` (`MeshFlags::Skinned`, `MeshSkinHeader`, `MeshSkin`, `MeshJoint`, `MeshSkinVertex`) | `test_render`: round trip, corrupt sections rejected (joint index past the skin, joint node past the tree, missing skin, stray offset) |
| Skins in `MeshStore`; one interleaved `SkinnedMeshVertex` stream for skinned meshes | `mesh.h` / `mesh.cpp` | `test_render` "the skin section round-trips" |
| Joint palettes and posed bounds | `skin_palette`, `skin_bounds`, `Scene3D::joint_palette()`, `Draw::palette` | `test_render` "the palette is the identity at rest and follows a bent joint", "Scene3D builds one palette per skinned instance; instances can opt out" |
| GPU skinning in the forward and shadow passes | `shaders3d.h` (`kSkinnedMeshVS`, `kSkinnedShadowVS`), four skinned pipelines in `Forward3D`, palette SSBO at `rhi::bank::kJointPalette` (7) | `test_render_gl` "a skinned bone bends with its joint; skinning off draws the bind pose", zero GL validation errors |
| Playback events | `ClipEvent`, `AnimationPlayer::events` | `test_render` "playback events fire once per pass, across loops, for the current clip only" |
| Sample | `04_cubes`: three skinned reeds (`models/reed.glb`, 4-joint chains, clip "sway"); `G` / `-skin 0` turns the skeletons off | screenshots below; the joint palette is in the headless determinism hash |

Screenshots (project files): `m-skin/reeds-skinned.png`, `m-skin/reeds-skeleton-off.png`.

## Design notes

- **Joints are nodes.** glTF skins name nodes as joints, so the existing node pose (`pose_nodes`, `pose_nodes_blend`,
  `AnimationPlayer` cross-fades) already animates skeletons. Skinning adds only the last step:
  `palette[j] = pose[joint node] * inverse_bind[j]`, the vertex being `Σ weight_j · palette[j] · bind position`.
- **The skinned node's transform is ignored**, as glTF specifies: skinned geometry lives in bind space and is placed
  by the instance's world matrix and its joints.
- **Skins ride in `CHNK_MESH`** (minor 1) rather than a separate chunk: like ADR-0022's clips, a skin only ever
  binds the nodes of the file it came from. Static meshes still cook to the minor-0 layout byte for byte.
- **One vertex stream.** The RHI binds one vertex buffer per draw, so a skinned mesh uploads an interleaved
  24-byte `SkinnedMeshVertex` (the 16-byte quantized vertex + 4 joint indices + 4 weights) and all its submeshes
  draw through the skinned pipelines; its rigid submeshes take the identity skin (`o_skin.x = -1`).
- **One palette upload per frame.** `Scene3D` concatenates every skinned instance's palette; `Forward3D` uploads
  it once with `set_storage_data` before the shadow pass, and each draw carries its first joint in `ObjectU`.
- **Culling bounds follow the pose.** Each joint stores the bind-space box of the vertices it moves; the posed
  bounds are the union of those boxes under the palette. A blended vertex lies in the convex hull of its joints'
  moved positions, so the union is conservative.

## Not built

Morph targets, more than 4 influences (`JOINTS_1`), dual-quaternion skinning, GPU palette compute, an editor
skeleton preview and an ECS animation component (3D rendering is host-driven today, like the samples). Ashen Court
keeps its node-transform proxies: skinning is available to it, not required by it.
