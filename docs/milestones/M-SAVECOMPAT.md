# M-SAVECOMPAT — Save and replay formats are pinned by frozen bytes (milestone record)

**Status:** done (one step). **Owner: the "Geliştirme rutini 3" thread.** Defined 2026-10-28 on the coordinator's suggestion (save/load robustness; device-free, no gameplay, outside M-WASTED and M-VISUAL). No Linear issues (Linear is at its free issue limit); this record is the tracker.

**State found.** Hostile and truncated save and replay bytes are already covered (M-FUZZ: `test_fuzz_player_data`, and the corrupt/truncated/newer-version cases in `test_save.cpp`), and a save from another game module version loads with counts instead of failing. What no test pins is the *layout*: every save test writes with the current code and reads it back with the current code, so a change to the header, the record order, the type hash or the field encoding passes all of them while every save file already on a player's disk stops loading.

**Design.**
- `tests/ecs/test_save_golden.cpp` carries a v1 save (two keyed entities, a `[Save]` struct and a counter) and a v1 replay as byte arrays, copied from the writer's output.
- `write_save` and `write_replay` must reproduce those bytes exactly, and `read_save` and `read_replay` must load them with the known values (no unknown, unknown-field or mismatched-field counts).
- A deliberate format change bumps `kSaveFormatVersion` or `kReplayFormatVersion` and keeps a v1 array that still loads; the failing test prints the new bytes for the array it replaces.
- Test only: no engine source or header changes.

## Steps
| # | Step | State |
|---|---|---|
| 1 | Frozen v1 save and replay bytes: write equals golden, golden loads | done |
