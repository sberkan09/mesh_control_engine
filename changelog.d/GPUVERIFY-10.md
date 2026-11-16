- Real-GPU round r10 (M-GPU-VERIFY): RTX A2000 against master 93859201, for M-GI2. The GI cases and the goldens pass on
  GL with llvmpipe's numbers (66 of 66 goldens, `gi_thin_wall` included). At 1080p the city's following volume costs +4.5 ms
  of GPU time at High, visibility +0.8 ms on top; the fill is 1 probe a frame (767 frames, about 30 s), so the 16 x 3 x 16
  grid stands. One finding: the `gi_vis` shader-against-CPU test fails on NVIDIA SDL GPU only; the city also crashes in
  SpriteBatch and CommandBuffer with the committed component (recorded, not fixed). Metal and WebGPU (A30) stay open.
