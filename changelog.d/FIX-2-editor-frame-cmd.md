- Editor: `mce_editor` records the frame's command stream on the heap and keeps its capacity between frames, like the
  player (#1215). Grown in the 16 MiB frame arena, every doubling of the stream left the old block behind, so a large
  scene with GI probe captures could overflow the arena.
