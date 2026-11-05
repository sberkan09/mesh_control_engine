- Windows test fixes, M-PLATFORM-CI step 4: the cook writes each output through a temp name of its own (process id and
  counter) and accepts a content-addressed file another process wrote first, so two editor servers cooking one project
  no longer fail with "cannot write cooked output"; the `playtest` event check matches MSVC type names, which carry a
  `struct ` prefix.
