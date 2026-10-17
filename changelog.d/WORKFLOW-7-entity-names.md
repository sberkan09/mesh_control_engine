- Scenes: `scene::find_entity(world, name)` finds a live entity by the name its scene gave it (the cook already kept
  the names; `instantiate` dropped them). Names live in the `EntityNames` resource, so no component is added and
  the state hash is unchanged; a scene change clears them. The Luau binding `mce.find` comes in a later step.
  (M-WORKFLOW 7, PR 9, #382)
