# M-HARDEN6 — Limits M-HARDEN4 listed and did not test: the asset cook (milestone record)

**Status:** step 1 done, step 2 in progress, started 2026-11-09. **Owner: the "Geliştirme rutini 3" thread.** Defined 2026-11-09 because the Linear Backlog holds only device-bound work (SBA-112, SBA-58) and M-HARDEN5 is done. No Linear issues. Test-only unless a test finds a bug; does not touch the render, rhi, kit (GTA and weather), physics3d or openworld modules that other threads own.

**Why.** M-HARDEN4 step 2 listed `kMaxShaderDefines`, `kMaxMeshLods` and `kMaxSkinJoints` as not done. All three sit in the asset cook and its loader (`formats.cpp`), so a test at N and N+1 needs no GPU or device.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `kMaxShaderDefines`: six defines cook all 64 variants, a 23-byte define name is kept and 24 is refused (`test_cook_shader.cpp`) | done (#1190) |
| 2 | `kMaxMeshLods` (`lod_levels` 3 cooks, 4 and -1 are refused, `test_cook_lod.cpp`) and `kMaxSkinJoints` (the cook refuses past the limit and the loader rejects a forged header at N+1) | `kMaxMeshLods` in progress, `kMaxSkinJoints` open |

**Gates:** each step's tests run in the module's own test binary (`test_cook`), so `--task` runs them.
