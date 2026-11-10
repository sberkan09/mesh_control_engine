- Real-GPU round r11 (M-GPU-VERIFY): RTX 3070 (driver 580.65.06) against master 2d116c4b, for M-CLOUDSHADOW (row A31).
  The 15 cloud shadow cases pass on NVIDIA GL, NVIDIA Vulkan, llvmpipe and lavapipe with the same measurements, and the
  `tour_cloudshadow` capture matches llvmpipe to 0.00% of pixels. The pass costs 0.054 ms (Medium) and 0.076 ms (High) at
  1080p, 0.037 / 0.041 ms at 720p, and nothing on Low. One test-data finding: the `cloud_shadow` golden fails on NVIDIA
  SDL GPU (3 of 300 blocks, worst 0.955%) because the committed lavapipe golden is about 1% darker than the other three
  renderers in two far-ground blocks (recorded, not fixed).
