- Fuzzing covers saves, replays, input recordings, replication, rollback and relay packets and Luau source, M-FUZZ
  step 3a (SBA-187): `test_fuzz_player_data`, `test_fuzz_net` and `test_fuzz_script` found no crash in those paths.
  They did find that Luau's bytecode loader trusts its input (an assert in debug, an out-of-bounds read in release);
  the case is skipped unless `MCE_FUZZ_BYTECODE=1` and the options are listed in `docs/milestones/M-FUZZ.md`.
