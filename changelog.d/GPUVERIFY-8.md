- Real-GPU round r8 (M-GPU-VERIFY): RTX A2000 against master 747a0d43, for M-REFLECT2. The SSR blur, bump widening and velocity-hit
  tests, all of `test_render_gl` and the goldens pass on GL and SDL GPU (goldens 3 of 3 runs each, 0 flakes) and match llvmpipe
  (0.00 to 0.03% differing pixels). At 1080p `r.ssr.blur` costs 0.02 to 0.03 ms (0.10 ms with a rough floor filling the lower frame)
  and `r.ssr.motion` nothing measurable. A roughness ladder recommends a default `ssr_max_roughness` of 0.6 (not changed; numbers in
  `docs/milestones/M-REFLECT2.md`).
