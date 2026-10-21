- Scripts: `mce.find(name)` returns the entity a scene gave that name, or `nil` (the first live one in scene order if
  several share it; entities spawned at run time have no name). It reads the `EntityNames` resource, so it changes
  nothing and a run hashes the same. (M-WORKFLOW 7, PR 10, #382)
