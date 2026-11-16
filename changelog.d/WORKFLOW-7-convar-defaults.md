- Console: `console::set_defaults(path)` makes the `name value` lines of a file the convars' defaults, and the player
  reads `defaults.cfg` beside its pack (or `-defaults <path>`) before the saved options load, so a project can
  ship its own defaults while a player's saved choice and the command line still win and a default is never written to
  `settings.cfg`. (M-WORKFLOW 7, PR 4a, #382)
