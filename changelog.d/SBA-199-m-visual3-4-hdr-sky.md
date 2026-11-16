- HDR sky and image-based light, M-VISUAL3 step 4 (#409): `.hdr` (Radiance) and `.exr` (OpenEXR, tinyexr) cook to a linear
  RGBA16F texture that keeps values above 1 (clamped to 65504, NaN/Inf to 0, mips averaged in float); the sidecar takes
  `max_size` (default 2048, a power of two), `exposure` (EV) and `mips`, and `"srgb": true` on an HDR file is a cook error.
  `Environment3D.sky_texture` and `ibl_texture` take them as is. `Environment3D.sky_rotation_deg` turns the sky texture and
  its image-based light about +Y (an HDRI aligned by hand; 0 draws the same frame as before). New tinyexr dependency,
  cook-only, behind its own wall.
