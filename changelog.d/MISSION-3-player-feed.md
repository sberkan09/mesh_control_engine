- Minimap fed by the stock player, M-MISSION step 3b: every visible `UiScreen` document with `Minimap` widgets gets the
  scene's map markers (`kit::collect_map_markers`) and the tracked entity's place (`kit::map_tracked_view`, by the widget's
  `track`) each frame, so a mission scene with a `.mcui` Minimap shows its objective markers with no code. Documents
  without a Minimap cost nothing. `src/player/src/minimap_feed.cpp`, `tests/player/test_minimap_feed.cpp`, user guide 9.15.
