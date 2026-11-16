- BC1 sRGB capability bit, M-CONTENT 1b follow-up (GH-387): `rhi::DeviceCaps::texture_bc_srgb` says whether BC1_SRGB is
  sampleable (GL: `GL_EXT_texture_sRGB` or `GL_EXT_texture_compression_s3tc_srgb`; SDL GPU: the BC1 sRGB format query;
  WebGPU and Metal: follows `texture_bc`). The texture store now refuses a BC1 sRGB pack on a device without it, once-logged,
  naming the fix (re-cook for this target, or `compress: bc7`), instead of failing in the driver. BC7 sRGB stays gated by
  `texture_bc`. The GL init warning is gone.
