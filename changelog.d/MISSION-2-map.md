- Map markers and the minimap, M-MISSION step 2: `MapMarker` (icon, colour, layer, `always_visible`, `follow_objective`;
  a bare marker on an `Objective` or `Mission` shows the active objective's target and goes away when it is done) in
  `mce/kit/map_markers.h`, and a `Minimap` UI widget kind (`UiMinimap`: tracked entity, radius, zoom, north-up or
  heading-up, rim clamping, background, full-map mode) in `.mcui` documents, drawn with the ordinary 2D quads, with the
  pure `ui::minimap_project` behind it. Documents without a Minimap are byte for byte what they were. User guide 9.15.
