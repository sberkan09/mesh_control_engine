- Windows: `test_cook` "fbx: a 3-bone skin with a rotation clip" passes (M-PLATFORM-CI step 4): the test's FBX writer stored
  key times (46186158000 ticks per second) in `long`, which is 32-bit on Windows and wrapped the one-second key to a wrong
  time. It uses `long long` now. Test-only; the importer was correct.
