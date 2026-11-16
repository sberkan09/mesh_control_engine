- Render: `render::SnowCover` gains `wind_dir` (xz, default +x), normalized to unit length by `apply_quality`
  (NaN, inf or a near-zero vector fall back to +x). Data seam only: no shader reads it yet, every frame is unchanged.
  (M-WINDDIR 8b)
