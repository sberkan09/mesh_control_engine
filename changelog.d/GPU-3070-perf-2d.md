- `-perf_capture` records `gpu_ms` for every frame (2D, split screen and overlays included): the player brackets
  the whole frame's command buffer with two GPU timestamps, where only the 3D stages had times before. The 3D
  stage columns are unchanged.
- `mce_player -split_views N` (2 to 4) draws the scene's primary 3D camera in N split-screen cells, for
  measuring GPU time against view count (e.g. `kit3d_tour`). A scene whose primary camera already has a
  `CameraView` keeps its own views.
