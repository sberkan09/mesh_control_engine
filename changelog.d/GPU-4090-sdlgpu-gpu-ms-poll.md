- SDL GPU `gpu_ms` resolves short frames: the fence waiter polls a fence for up to 2 ms before it blocks, so its
  stamps are microseconds late, not tens of microseconds. 2D frames on an RTX 4090 read p50 0.001 ms before (GL:
  0.008-0.026 ms), because their GPU work finished within the old wake-up lateness of the previous fence.
