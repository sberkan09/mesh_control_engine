- SDL GPU backend (`MCE_RHI=sdlgpu`): `-perf_capture` now records `gpu_ms`, which was always 0 samples there. SDL GPU has no
  timestamp queries, so the device stands a fence in for each timestamp written outside a pass (`DeviceCaps::fence_timestamps`)
  and a waiter thread stamps the CPU time each one passes, with the GPU's idle time between submits taken out. The value is
  an upper bound of the GPU time (the wake-up latency is included, tens of microseconds). Forward3D's per-stage times stay
  GL/Metal only: each timestamp splits the submit, so only a measuring run's two frame timestamps use it.
