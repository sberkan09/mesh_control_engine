- MCP `render_frame` / `render_sheet` with `"software": true` (and on a box with no display) now really draw with Mesa's llvmpipe on a
  machine that also has NVIDIA's EGL/GLX: besides `LIBGL_ALWAYS_SOFTWARE`, which only Mesa reads, the player is started with libglvnd
  pointed at Mesa (`__EGL_VENDOR_LIBRARY_FILENAMES`, `__GLX_VENDOR_LIBRARY_NAME`). Before, the "software" frame came from the GPU there.
  Two GPU tests no longer assume llvmpipe: the storage-image test writes a blue that is not a rounding tie, and the MCP software frame is
  compared to the window's within 0.5 % instead of byte for byte (first real-GPU run, RTX 3070).
