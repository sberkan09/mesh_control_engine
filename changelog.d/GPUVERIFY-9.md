- Real-GPU round r9 (M-GPU-VERIFY): RTX A2000 against master 2d17e046, for M-WATER2. The water, underwater and water-optics
  tests and all of `test_render_gl` pass on GL and SDL GPU with llvmpipe's numbers, and `tour_river` and `13_water` match
  llvmpipe and lavapipe (0.00 to 0.01% differing pixels). At 1080p both water effects cost +0.59 ms (High) and +0.45 ms
  (Medium) of the main pass on `tour_river` and +0.94 / +0.68 ms on the `13_water` sea, mostly the reflection march; the pass
  break and copy stay at 0.07 to 0.11 ms. An intermittent `VK_ERROR_DEVICE_LOST` in the SDL GPU goldens (4 of 15 runs, on
  cases without water, not reproduced in the last 8) is recorded in `docs/milestones/M-GPU-VERIFY.md`.
