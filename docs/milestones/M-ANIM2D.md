# M-ANIM2D — 2D skeletal animation (milestone record)

**Status:** active (step 1 in progress).
**Why:** 2D characters animate only by sprite frames today; cut-out rigs (bones, blended clips, skinned meshes, IK) are what Godot's Skeleton2D, Unity's 2D Animation and Spine give 2D games. Added 2026-10-16 under the user's standing instruction to extend the roadmap when no milestone is free.
**Plan:** ROADMAP § M-ANIM2D.
**Measured on:** the Linux CI container.

## Steps

| Step | Linear | State |
|---|---|---|
| 1. `.mcskel2d` assets, `Skeleton2D` world pose, slots drawn through the sprite extraction | SBA-140 | in progress |
| 2. Bone clips and crossfade, driven by the `.mcanim` state graph | SBA-141 | — |
| 3. Mesh attachments and CPU skinning, deform keys | SBA-142 | — |
| 4. Two-bone and CCD IK as data | SBA-143 | — |
| 5. Editor skeleton panel and MCP `skel2d_*` tools | SBA-144 | — |
| 6. Demo, user guide, gates | SBA-145 | — |
| Later: Spine / DragonBones JSON import | SBA-146 | not scheduled |
