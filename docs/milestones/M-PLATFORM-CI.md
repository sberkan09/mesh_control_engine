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
| 4a. `test_core` (group 4): fixed in PR #1098, it passes on the hosted runner. Cause was not `PlatformInit` (that line is a test that fails `on_init` on purpose) but the convar settings tests writing to the nonexistent `/tmp` | done |
| 4. Windows `ctest`: the build is green; 17 of 201 tests fail and two hung the first full run until the 4 h job limit (ctest now has `--timeout 600`, jobs 150 min) | open: `import_demo_reimport`, `sample_steam_demo`, `ai_demo_editor_bt`, `Dungeon_perf_counters`(+`_seeded`), `marble_garden_mctests`, `test_core` (0xc0000409), `test_cook`, `test_fuzz_script`, `test_fuzz_importers`, `cli_test`, `test_script`, `player_scripts`, `test_editor`, `mcp_transcript`, `test_mcp_playtest`, `test_mcp_agent`; each needs a look |
| 5. macOS test failures below | `sdk_install` and `sample_steam_demo` fixed on master; `test_kit` and `test_mcp_agent` open |
| 6a. Android APK build (`android.yml`: host `mce_cook`, NDK r28, Gradle, debug-signed APK as an artifact) | done, PR #1097 (green on the first run, 19 min) |
| 6b. Web build | already in the integration run (`tidy-web` group runs the `web` stage) |
| 6c. Release packages (`release.yml`: Ember Step zip for Linux, Windows and macOS; on a `v*` tag attached to a GitHub release) | PR open; first run decides |
| 6d. GitHub Pages for the docs (`pages.yml`, MkDocs) | PR open; the build runs on the PR, the deploy job waits for Settings > Pages > Source "GitHub Actions" and the repository variable `PAGES_ENABLED=true` |

## Known macOS failures on the hosted runner (204 of 208 pass)

The runner is a VM without a Metal GPU, so some of these may be environmental; each needs a look before it is
called so. None is skipped.

- `test_kit`: the `*_extract` cases (butterfly, cold breath, dusk bat, dust, flock, midge, snow sparkle) read `0` where
  the emitter should follow the camera (`position == 30`), and `test_move3d` lap times differ by 0.63 s (limit 0.5 s).
- `test_mcp_agent`: `render_frame` returns no image, then SIGSEGV in the split-screen case (probably no Metal device).
- `sample_steam_demo`: the test always built the `linux-x64` depot, so on macOS (and Windows) the player never found its steam_api library. Fixed: the test builds the host platform's depot.
- `sdk_install`: newer CTest prints `100% tests passed out of 1` without `, 0 tests failed`; the check accepts both. Fixed.
- `test_kit` extract cases: not fixed here. Position reads `0`, so the camera pose the emitter follows is not reaching the extract on macOS; needs a Mac to reproduce (kit module, separate PR).
- `test_mcp_agent`: environmental. The player's `offscreen` SDL driver cannot create a Metal window on the VM (`SDL_CreateWindow failed: Metal support is either not configured ...`), and the test then SIGSEGVs after its `REQUIRE` fails. Needs the render tests labelled `gpu` or skipped without a device (mcp module, separate PR).

## Linear (to open when it is reachable)

One issue per step 1-6 under project mesh_control_engine, step 1-3 Done when #1088 merges.

## Windows test failures, grouped (first full run, 2026-11-04; 17 of 201, plus two hangs)

Groups are by module so concurrent threads can take them without overlap. The evidence is from the run's log.

1. **Luau compiler crashes (script).** `test_fuzz_script` and `test_script` ("a syntax error fails the cook at file:line:col") die with SIGSEGV; `player_scripts` gets an access violation from `mce_cook --pack` when a script has an error. One cause is likely: the compiler's error path.
2. **Paths with backslashes (asset, mcp, tests).** `import_demo_reimport` reports every asset path as changed (`assets\levels\cave.mcscene`); `cli_test` (Python driver) and `mcp_transcript` ("cannot write cooked ...") fail on paths; `test_editor` and `ai_demo_editor_bt` probably too.
3. **FBX import (asset).** `test_cook` "fbx: a 3-bone skin with a rotation clip": the sampled quaternion differs from the analytic one (`0.707` against `0`).
   Cause found: the test's FBX writer (`FbxDoc::curve`) stored key times in `long` (32-bit on Windows); one second is 46186158000 ticks, so the key at 1 s wrapped. Fixed with `long long` in the test; the importer was fine. Needs the next Windows run to confirm.
4. **Needs a display or audio device (platform).** `test_core` fails `PlatformInit` and then aborts with 0xc0000409; check whether the test should skip without a device or the runner needs one. `sample_steam_demo` expects the Linux depot in its log line.
5. **Perf gates (kit).** `Dungeon_perf_counters(_seeded)` and `marble_garden_mctests` never print `gate: pass`; the hosted runner may simply be too slow, measure before changing a budget.
6. **Hangs.** Two tests had not finished when the first run hit the 4 h limit (`test_mcp_openworld` took 48 s; the stuck ones started after `test_mcp_team`). ctest now has `--timeout 600`, so the next run names them.
