# M-VISUAL3 — What keeps the engine from an AAA look with data alone (milestone record)

**Status:** not started (added 2026-10-16). Plan: ROADMAP §M-VISUAL3; Linear SBA-199 (step 1; steps 2 to 6 not filed, see below). Source: GitHub #428.

**Why:** blind test 3 (rounds 5 and 6) had an art agent dress a world with real scanned and low-poly content through
data and MCP only and rate it about 4/10 against an AAA bar. #428 lists what stopped it: no parallax or displacement,
no vignette, grain, chromatic aberration or depth-of-field control, an 8-bit sky with no `.hdr` / `.exr` importer (#409),
no texture compression (M-CONTENT 1), no static merge or instancing for `MeshRef`, a thin `Foliage3D`, and limits on
probes and decals.

**Model policy (coordinator, 2026-10-16):** the render pipeline is Opus work. Step 1, an Opus thread, writes the plan and
the interfaces and opens them as a PR for review; the other steps are Sonnet and follow that note, with an Opus review of
any change to the shaders or the draw path.

| # | Step | Linear | Model | State |
|---|---|---|---|---|
| 1 | Plan and interfaces: #428 checked against the code, fields, keys, arguments, cost | SBA-199 | Opus | not started |
| 2 | Parallax occlusion mapping from a height map on `.mcmat` | not filed | Sonnet (Opus review) | not started |
| 3 | Filmic post chain: vignette, grain, chromatic aberration, depth-of-field focus | not filed | Sonnet (Opus review) | not started |
| 4 | `.hdr` / `.exr` sky and image-based light (#409) | not filed | Sonnet | not started |
| 5 | Static merge and instancing for `MeshRef` | not filed | Sonnet (Opus review) | not started |
| 6 | Foliage variation, the demo, gates and the close | not filed | Sonnet | not started |

**Not in scope:** texture compression and shipped size (M-CONTENT 1); more than one `GiVolume3D`; retargeting, IK,
cloth, hair; subsurface scattering; light shafts; a 3D importer beyond glTF.

**Linear:** the workspace hit Linear's free issue limit on 2026-10-16 while step 2 was being filed, so only step 1 (SBA-199) exists. Steps 2 to 6 are filed when the limit is lifted or old issues are archived; until then this table is the record.

**Gates:** existing replay hashes unchanged with every feature off by default; each step has a regression test and an
on/off render; `local_ci.sh` green on every step.
