- Real-GPU test fixes from the RunPod RTX 4090 / Blackwell round: the golden check looks in `<os>-<backend>-nvidia/` first
  when the device is NVIDIA, which holds the `ibl_roughness` golden NVIDIA renders identically on three GPUs (one block
  of 4x MSAA box edges 0.97% from llvmpipe); `editor_play_smoke` runs one fixed tick a frame, so a fast GPU no longer
  ends its 90 frames before the first 60 Hz tick; `player_capture_no_display` also unsets `SDL_VIDEO_DRIVER`, the SDL 3
  spelling, so a box that sets it still exercises the offscreen fallback.
