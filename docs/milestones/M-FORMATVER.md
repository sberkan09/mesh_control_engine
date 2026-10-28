# M-FORMATVER — One version check for every asset text (milestone record)

**Status:** open (one step). **Owner: the "Geliştirme rutini 3" thread.** Defined 2026-10-28 on the coordinator's suggestion (serialization versioning; device-free, no gameplay, outside M-WASTED and M-VISUAL). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**State found.** The two suggested areas are mostly done: scene and prefab migration exists (M-SCENEMIGRATE: component and field rename tables, #799 and #802) and the performance tooling with its counter gate exists (M-PERF). What is left in serialization is the cook side: 19 asset text importers, 20 checks (config, roads, services, tilemap, defaults, behavior tree, animation graph, sequence, input map, timeline, terrain, sprite sheet, particles, ui, ui theme, sound set, skeleton, material, items, store) each carried its own copy of `version != current -> "unsupported ... version"`. The day a format goes to version 2, every version 1 file in a game project would be refused by that check, with an error code that says "too new" for a file that is too old.

**Design.**
- `detail::check_format_version(doc, current, what, file, stage, diags)` in the cook-internal `importer_common.h`: the version is an integer from 1 up to `current`. A file older than `current` passes, so a bump keeps its old files loading and adds its migration in that importer; anything else reports the same `unsupported <what> version (this cook reads version N)` text with `Errc::VersionTooNew`.
- The 19 importers call it instead of their copy. Behavior is unchanged today (every format is at version 1; the animation graph already accepted a range). `scene_importer.cpp` keeps its own check: its message tells the user to re-save in the editor, and scene migration lives in `SceneDocument`.
- `importer_common.h` is included by 34 translation units, all in the cook; no public header changes.

## Steps

| # | Step | State |
|---|---|---|
| 1 | `check_format_version`, the importers use it, a `test_cook` case for the helper | open (PR) |

**Gates:** `test_cook` (the whole binary), the include-fanout wall.
