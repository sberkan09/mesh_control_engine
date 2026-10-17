- UI: the convar `ui.reference_height` (default 0 = off) makes `mce_player` grow the UI with the window's short side
  over the height the UI was authored for, so a UI built for 720 is not tiny at 1080p; the display's own scale still
  wins where it is larger. `ui::compute_ui_scale` takes the height as an optional last argument. (M-WORKFLOW 7,
  PR 2, #382)
