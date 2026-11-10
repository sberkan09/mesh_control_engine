- Player: the city sample no longer crashes with its GI volume (GPU round r10). `mce_player` recorded the frame's
  command stream in the 16 MiB frame arena, where every doubling of the stream left the old block behind: the city's
  2 MB stream with the GI probe captures took about 7 MB, the frame passed 16 MiB once HUD text or a free camera added
  a little more, and the null allocations crashed in `SpriteBatch` and `CommandBuffer`. The stream now lives on the
  heap and keeps its capacity between frames (the city's frame arena peak over a 400-frame Medium drive: 8.9 MB, where
  it overflowed at frame 101 before). New `-mem.frame_arena_mb` (default 16) sizes the frame arena;
  `openworld_frame_arena_gpu` checks that the city's frame fits in 12 MiB.
