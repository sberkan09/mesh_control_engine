- `sprites_2d -sprites 50000` no longer crashes (first real-GPU run): the sample sizes the frame arena (~540 bytes a
  sprite) and the geometry ring (96 bytes a sprite) from `-sprites`. A `LinearArena` overflow now prints one line
  in every build (which arena, how much was asked, its size) instead of a silent crash where asserts are off.
