# M-PLATFORM-CI: Windows and macOS on hosted runners

Added 2026-11-04 after the repository went public (hosted runners are free). No Linear issue yet: the Linear MCP
server was unreachable when this was written; open the issues when it is back (steps below).

`.github/workflows/platforms.yml` builds and tests `msvc-release` (`windows-2025`) and `macos-release` (`macos-26`)
nightly, on demand (`workflow_dispatch`) and on a PR that edits the workflow. It is not a PR gate. `gpu` and `slow`
tests are excluded: hosted runners have no usable GPU. Linux stays the gate (`ci.yml`, `integration.yml`).

| Step | State |
|---|---|
| 1. Workflow, Windows and macOS jobs | PR #1088 |
| 2. First-run build fixes, Windows (C4702 after `luaL_error`, module host headers for UMBRELLA targets without the umbrella, `net_arena` deps and `<algorithm>`, C4310 `char` casts, `_putenv_s`, `std::popcount`) | PR #1088 |
| 3. macOS: `macos-26` runner (Xcode 16.4's libc++ has no floating-point `from_chars`) | PR #1088, builds green |
| 4. Windows `ctest` | open until the build is green |
| 5. macOS test failures below | open |
| 6. Android APK build, web build, release artifacts, Pages | not started |

## Known macOS failures on the hosted runner (204 of 208 pass)

The runner is a VM without a Metal GPU, so some of these may be environmental; each needs a look before it is
called so. None is skipped.

- `test_kit`: the `*_extract` cases (butterfly, cold breath, dusk bat, dust, flock, midge, snow sparkle) read `0` where
  the emitter should follow the camera (`position == 30`), and `test_move3d` lap times differ by 0.63 s (limit 0.5 s).
- `test_mcp_agent`: `render_frame` returns no image, then SIGSEGV in the split-screen case (probably no Metal device).
- `sample_steam_demo`: expects the Linux depot in its Steam log line.
- `sdk_install`: not yet read.

## Linear (to open when it is reachable)

One issue per step 1-6 under project mesh_control_engine, step 1-3 Done when #1088 merges.
