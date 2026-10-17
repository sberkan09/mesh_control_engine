- Cook: a scene or prefab is re-cooked when the C++ default of a component field it omits changes
  (the scene importer's `schema_hash()` now includes the default-constructed bytes of each type; before, only
  the field layout counted and a baked old default stayed in `cooked/`). (M-WORKFLOW 6b, #391)
