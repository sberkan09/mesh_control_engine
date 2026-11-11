- Real-GPU round r12 (M-GPU-VERIFY): RTX A2000 (driver 550.127.05) against master f20a5e77, for M-CAUSTICS (step 52e,
  row A32). The 16 caustics cases and the 36 water, underwater and cloud shadow cases pass on NVIDIA GL, NVIDIA Vulkan,
  llvmpipe and lavapipe; the `caustics` golden passes on both NVIDIA backends (worst block 0.088%) and the `tour_caustics`
  capture matches llvmpipe to 0.00% of pixels over 12/255. The pass costs 0.52 ms (Medium) and 1.03 ms (High) at 1080p,
  0.24 / 0.46 ms at 720p, and nothing on Low. One look finding, the same on every renderer: a light fringe where the water
  shows the reflected wall next to the refracted floor (0.29% of the frame), because the web is weighted by the flat
  plane's Fresnel where the water shows a reflection (recorded with its fix, not fixed). M-CAUSTICS is closed.
