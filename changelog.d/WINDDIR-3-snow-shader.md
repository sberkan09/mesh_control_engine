- Render: the snow's lee bias follows `render::SnowCover::wind_dir` (`dot(ng.xz, wind_dir)` in place of the
  hard-wired `ng.x`), uploaded as the new `p_snow2` pass uniform (written only when cover > 0). The default (1, 0)
  draws byte-identical frames (every existing golden unchanged, `snow_plaza` included); a wind toward -x moves the
  extra snow to the -x-facing slopes. New golden `snow_plaza_west` (an even layer at coverage 0.6, wind -x).
  (M-WINDDIR 8c)
