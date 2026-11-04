# M-HARDEN3 — Error paths and encode/decode pairs the tests never exercise (milestone record)

**Status:** step 1 open. **Owner: the "Geliştirme rutini 3" thread.** Defined 2026-11-04 on the coordinator's instruction to start a new hardening milestone when the name-based survey of M-HARDEN2 step 4 runs dry. No Linear issues (the Backlog holds only the device-bound SBA-112 and SBA-58); this record is the tracker.

**Why.** M-HARDEN2 step 4 closed every public function whose name no test mentions, outside the modules that other threads own (weather, GTA gameplay, openworld, render, rhi). A name in a test only proves the function runs once on a happy path. Two kinds of code can still hide bugs behind it: the failing branch of a `Result`-returning function that no test provokes, and the half of an `encode_*` / `decode_*` pair that a test calls only in one direction.

**Scope.** Device-free and GPU-free, test-only unless a test finds a bug. Outside the modules above. A fix needs its own regression test beside the module's tests, and a fix to core production code waits for the coordinator's review.

## Steps

| # | Step | State |
|---|---|---|
| 1 | Survey: for each `encode_X` / `decode_X` (and `write_X` / `read_X`) pair in the public headers, whether one test calls both on the same value and checks equality; list the pairs where it does not | open |
| 2 | Round-trip tests for the pairs step 1 lists, one cluster per PR, with a damaged-input case (a cut buffer, a flipped length) that must return an error and not read past the end | open |
| 3 | Survey: `Result<T>` / `bool`-returning public functions whose error branch no test reaches (a branch the code documents, such as "false when the id is empty or already used") | open |
| 4 | Error-path tests for what step 3 lists, one cluster per PR | open |

**Gates:** each step's tests run in the module's own test binary, so `--task` runs them. Any bug a step finds is fixed with a regression test and recorded in this table.
