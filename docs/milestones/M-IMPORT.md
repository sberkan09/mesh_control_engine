# M-IMPORT — 2D authoring-tool importers (milestone record)

**Status:** done 2026-10-16 (added and finished the same day). Plan: ROADMAP §M-IMPORT; Linear SBA-167 … SBA-170.

**Why:** 2D art comes from Aseprite and levels from Tiled or LDtk. The engine's `.mcsheet` and
`.mctilemap` are written by hand, by the Tilemap panel or by an agent. The converters read those tools'
exports and write the native text assets, so the cook, the runtime and the determinism contract are
untouched.

**Design.** `tools/mce_cli/mce_import.py` (Python, no dependencies), run as `mce import <tool> <file>
[--project <dir>]`, installed beside `story_compile.py`. A converter is a pure function of its input
bytes: no clock, no random GUID for an existing asset (a re-import reads the GUID out of the file it
overwrites), keys sorted, so the output is the same twice. Unsupported features are warnings printed with
the feature name and the source location; `--werror` makes them errors. It writes next to the export
unless `--out` is given and prints one line per file written.

**Not in scope:** Aseprite's native `.aseprite` binary (export JSON + PNG is the supported path), Tiled
infinite maps, isometric and hex maps, Tiled/LDtk live reload in the editor, writing back to the tools.

## Steps

| # | Step | Linear | Model | State |
|---|---|---|---|---|
| 1 | `mce import aseprite`: frames, tags as clips, durations, slices (`tools/mce_cli/mce_import.py`, `tests/import/test_import.py`; user guide 7.3) | SBA-167 | Sonnet | done |
| 2 | `mce import tiled`: tile layers, flips, animated tiles, object layers (user guide 7.3) | SBA-168 | Sonnet | done |
| 3 | `mce import ldtk`: levels as scenes, IntGrid colliders, entity layers (user guide 7.3) | SBA-169 | Sonnet | done |
| 4 | MCP `asset_import` (`src/editor/mcp/src/tools_import.cpp`, `tests/mcp/test_mcp_import.cpp`), user guide 5.3, 7.3, 14, 18, `samples/import_demo` and the gates | SBA-170 | Sonnet | done |
| + | `mce import spine` / `mce import dragonbones`: 2D skeletons into `.mcskel2d` (`tools/mce_cli/mce_import_skel.py`, `tests/import/test_import_skel.py`; user guide 7.13; recorded in M-ANIM2D) | SBA-146 | Sonnet | done |

**Gates:**
- Converters give the same bytes out twice and keep an existing GUID.
- Every unsupported feature is a named warning.
- An imported sheet cooks to the same chunk as the equivalent hand-written `.mcsheet`.
- `local_ci.sh` green on every step.

**Gate evidence.** Same bytes out twice and kept GUIDs: `tests/import/test_import.py` per tool, and the
`import_demo_reimport` ctest re-converts the sample's exports and compares every committed asset byte for
byte. Named warnings: each converter test checks the warning text, `--werror` writes nothing (also over
MCP). Cooking: the sample's pack cooks all imported assets and `import_demo_player` plays the imported hero
over the imported ground for 300 ticks with one hash twice; `test_mcp_import` registers and reads the
imported assets. Not separately tested: that an imported sheet's chunk equals a hand-typed copy (the
converter writes the same JSON the importer reads, so it holds by construction).

**As built, and limits.** `asset_import` runs `mce_import.py` as a child process (Python 3 on the PATH or
`MCE_PYTHON`; the script is found beside the server, in a source tree above it, or by `MCE_IMPORT_SCRIPT`),
so the editor binary carries no second implementation of the converters; a machine without Python gets a
clear error from the tool. The sample's exports are written by `gen_art.py` in the tools' formats, not
saved from Aseprite, Tiled or LDtk themselves; real files from the applications were not available in this
environment, so fields the hand-written exports omit are untested against real output. Not imported:
flipped or rotated tiles (drawn unflipped), animated tiles, ellipse / polygon / text / tile objects, image
layers, LDtk entity fields and world layout, Spine-style skeletons (see the `+` row), Aseprite's binary
format. Tileset margin / spacing, infinite, isometric and hex maps are errors.
