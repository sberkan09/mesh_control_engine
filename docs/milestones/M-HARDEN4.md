# M-HARDEN4 — Documented capacity limits tested at N-1, N and N+1 (milestone record)

**Status:** done 2026-11-08 (steps 1 to 4); the leftovers are listed under steps 2 and 4. **Owner: the "Geliştirme rutini 3" thread.** Defined 2026-11-05 on the coordinator's instruction to start a new milestone when the Linear Backlog holds only device-bound work (SBA-112, SBA-58). No Linear issues. Does not touch the render, rhi, kit (GTA and weather), physics3d or openworld modules that other threads own.

**Why.** M-HARDEN3 tested error paths and encode/decode pairs. A third kind of code hides behind fixed-size arrays and `FixedString<N>` fields: the branch that clamps, drops or refuses at the limit. A happy-path test never reaches it, and a limit that is off by one (an unchecked write at `N`, a clamp that lets `N+1` through) is a buffer overrun, not a wrong answer. Each public `kMax*` constant is a documented limit; the test is to drive the code at `N-1`, `N` and `N+1` and check that nothing is written past the end and that the refusal or clamp is the documented one.

**Scope.** Device-free and GPU-free, test-only unless a test finds a bug. A fix needs its own regression test beside the module's tests, and a fix to core production code waits for the coordinator's review.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Survey: every `constexpr ... kMax*` in the public headers (outside the modules above) whose name no file under `tests/` mentions | done 2026-11-05: 43 constants, 31 with no mention (listed below). A name in a test is not proof (a test may use the literal), so step 2 reads the code that uses each constant before writing a test |
| 2 | Boundary tests for the listed constants, one cluster per PR: `N-1`, `N`, `N+1`, and the empty input | done: `kMaxSubtitleLines` (`max_lines` capped, 0 shows nothing, a 400-byte line is cut at the 256-byte display buffer) in `test_subtitles.cpp`; `kMaxTextBytes` (fills to 64 exactly, drops a 2-byte char that no longer fits), `kMaxTouchPoints` (oldest four of six fingers) and `kMaxActionLabel` (47 kept, longer cut, round-trips at the limit) in `test_actions.cpp`, `test_touch.cpp` and `test_options.cpp`; `kMaxParams` (25 accepted, the 26th refused) and `kMaxReportBytes` (exactly 1 MiB sent, one byte more skipped and kept) in `test_liveops.cpp`; the predictor's pending limit (256 at the limit and past it, newest kept) and `redundancy` clamp (0 and 1 send one input, 32, 33 and 1000 send 32) in `test_prediction.cpp`; `kMaxClips` (1024 registered, the next refused, a release frees one) and `kMaxHandles` (256 plays in one tick, the next dropped) in `test_device.cpp`; `kMaxMods` (256 load, the 257th skipped as "too many mods", with and without `load_order.txt`) in `test_mods.cpp`; `kMaxActions` in the input script (64 on a line parse, 65 is a parse error with its line; the 65th distinct action on a tick is dropped) in `test_script.cpp`; `kMaxPolygonVertices` (15 and 16 ring points hull in full, a 17th is cut) in `test_collision.cpp`; `kMaxDocumentWidgets` (4096 widgets load, 4097 refused as out of bounds) in `test_ui_screen_link.cpp`; `kMaxCalls` (32 Steam calls wait, the 33rd is refused) in `test_steam.cpp`; `kMaxSnapshots` (15, 16 and 17 snapshots blend -15, -16 and -16 dB) in `test_duck_snapshots.cpp`; `kMaxFeeds` (N-1, N kept, N+1 cut to the first N in order, an empty set clears) in `test_steam_config.cpp`. Not done, and why: `kMaxVirtualVoices` (mixer internal, no public way to reach it), `kMaxGamepads`, `kMaxLines` (`ecs/dev_stats.h`, no test target yet), `kMaxMeshLods`, `kMaxShaderDefines` and `kMaxSkinJoints` (asset cook), `kMaxComponents` (an assert, not testable), the kit novel constants (not this milestone's modules), the `perf_capture` constants |
| 3 | Survey 2: `FixedString<N>` fields set from caller-supplied text (names, ids, keys): does a longer text truncate, refuse or overrun | done: `FixedString::assign` truncates silently by design (`truncated()` reports it), so the question is whether the caller checks it. `save::write`/`read` do (refuse with `InvalidArgument`); relay modes and codes validate length. Left to test, one cluster per PR: scene `load_slot`/`next`, ui widget `name`/`text`, items `key`/`id`, `dev_stats` key. Open observation: `save::exists` does not check `truncated()` |
| 4 | Boundary tests for what step 3 lists, one cluster per PR | done: save slot name at the 512-byte path limit in `test_save.cpp` (fixed `save::exists`, #1166), scene `load_slot` at 63 bytes (fixed, #1168), ui widget name at 31 bytes in `test_ui.cpp` (#1171); items id/name at 63 bytes in `test_cook_items.cpp`. Follow-ups in M-HARDEN5 PRs: items stat name refused past 31 bytes (fixed, #1176), recipe station at 31 bytes, scene `next` past 255 bytes is NotFound and keeps the world. scene `next` test #1177. Checked, nothing to test: a widget's runtime `text` is an unbounded `Array<char>` (only the document file's 256-byte `text` field is bounded, covered by the screen link tests). Not done: `dev_stats` key (no test target for it yet) |

**Gates:** each step's tests run in the module's own test binary, so `--task` runs them. Any bug a step finds is fixed with a regression test and recorded in this table.

## Step 1 list (no test file mentions the name)

| Constant | Header |
|---|---|
| `kMaxActionLabel` | `mce/input/input_map.h` |
| `kMaxActions` | `mce/input/actions.h` |
| `kMaxCalls` | `mce/platform/steam.h` |
| `kMaxClips` | `mce/audio/audio.h` |
| `kMaxComponents` | `mce/ecs/entity.h` |
| `kMaxCounters` | `mce/kit/novel_components.h` |
| `kMaxDepth` | `mce/kit/novel_components.h` |
| `kMaxDist` | `mce/containers/hash_map.h` |
| `kMaxDocumentWidgets` | `mce/ui/document_data.h` |
| `kMaxFeeds` | `mce/input/input_system.h` |
| `kMaxFlags` | `mce/kit/novel_components.h` |
| `kMaxGamepads` | `mce/platform/event.h` |
| `kMaxGpuStages` | `mce/perf_capture.h` |
| `kMaxHandles` | `mce/audio/audio.h` |
| `kMaxLines` | `mce/ecs/dev_stats.h` |
| `kMaxMeshLods` | `mce/asset/formats.h` |
| `kMaxMods` | `mce/asset/mods.h` |
| `kMaxPacketInputs` | `mce/net/rollback.h` |
| `kMaxParams` | `mce/liveops/analytics.h` |
| `kMaxPending` | `mce/net/prediction.h` |
| `kMaxPhases` | `mce/perf_capture.h` |
| `kMaxPolygonVertices` | `mce/physics/components.h` |
| `kMaxReportBytes` | `mce/liveops/crash_upload.h` |
| `kMaxShaderDefines` | `mce/asset/formats.h` |
| `kMaxSkinJoints` | `mce/asset/formats.h` |
| `kMaxSlots` | `mce/kit/novel_components.h` |
| `kMaxSnapshots` | `mce/audio/components.h` |
| `kMaxSpikes` | `mce/perf_capture.h` |
| `kMaxTextBytes` | `mce/input/actions.h` |
| `kMaxTouchPoints` | `mce/input/actions.h` |
| `kMaxVirtualVoices` | `mce/audio/audio.h` |
