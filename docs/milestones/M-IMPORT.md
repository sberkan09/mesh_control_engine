# M-IMPORT — 2D authoring-tool importers (milestone record)

**Status:** active (added 2026-10-16). Plan: ROADMAP §M-IMPORT; Linear SBA-167 … SBA-170.

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
| 3 | `mce import ldtk`: levels as scenes, IntGrid colliders, entity layers | SBA-169 | Sonnet | not started |
| 4 | MCP `asset_import`, guide, sample, gates | SBA-170 | Sonnet | not started |

**Gates:**
- Converters give the same bytes out twice and keep an existing GUID.
- Every unsupported feature is a named warning.
- An imported sheet cooks to the same chunk as the equivalent hand-written `.mcsheet`.
- `local_ci.sh` green on every step.
