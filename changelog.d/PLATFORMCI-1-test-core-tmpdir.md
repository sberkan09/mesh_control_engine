- Windows: `test_core` no longer aborts (0xc0000409) in the convar settings tests; they wrote to the nonexistent
  `/tmp` instead of the platform temp directory (M-PLATFORM-CI step 4).
