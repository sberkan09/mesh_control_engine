- Parallax occlusion mapping draws, M-VISUAL3 step 2b: a material with a `height` map and `height_scale` (step 2a) now
  shows depth that shifts with the view on floors, walls and paths. It fades out at 30 m on High and 15 m on Medium
  and is off on Low (`QualitySettings::parallax_distance`, `parallax_max_steps`); materials without a height map draw
  byte-identical frames.
