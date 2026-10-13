# Versioning

The engine follows [Semantic Versioning](https://semver.org). The version lives in `CMakeLists.txt`
(`project(MeshControlEngine VERSION x.y.z)`); ship zips (`<Game>-x.y.z+<commit>`), new projects and every
`projects/*/*.mceproj` take it from there.

## When it moves

Until 1.0.0:

- **Minor** (0.9.0 → 0.10.0): a ROADMAP milestone closes, off-tier ones included. The PR that closes the
  milestone bumps the version.
- **Patch** (0.9.0 → 0.9.1): fixes or small features between milestones, bumped when a game build is shipped
  from them. Ordinary PRs do not bump.
- **1.0.0**: when the v1.0 gate's human-only checks pass (ROADMAP §M9).

From 1.0.0, **major** moves only for a break: a game module built against the old engine no longer loads or
builds, an old `.mceproj` or cooked asset no longer opens, or `05_showcase` (the API-stability specimen)
needs changes to compile.

## How

1. Every PR that changes what a user of the engine sees adds a line under `## Unreleased` in `CHANGELOG.md`.
2. The bumping PR runs `python3 scripts/bump_version.py x.y.z`. It updates `CMakeLists.txt`, the projects'
   `engine_version`, the user-guide example, and turns `## Unreleased` into `## x.y.z (date)`.
3. After that PR merges, tag the merge commit `vx.y.z`: `git tag -a vx.y.z origin/dev_claude && git push origin vx.y.z`,
   or on GitHub, Releases → Draft a new release → tag `vx.y.z`, target `dev_claude`. Cloud Claude sessions can
   push only their own branch, so they leave this step to the owner.

`scripts/ci/local_ci.sh` runs `bump_version.py --check` in its walls stage, so a copy that disagrees with
`CMakeLists.txt` fails CI.

The engine id that guards game modules (`cmake/EngineId.cmake`) comes from `git describe --tags`, so after a
tag, reconfigured trees get a new id and stale modules are refused until rebuilt.
