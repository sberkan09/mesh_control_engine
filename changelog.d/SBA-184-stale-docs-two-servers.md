- Stale documents and the close of M-WORKFLOW step 5 (SBA-184, #381): the user guide now describes `ParticleEmitter3D`, the SDK form of
  shipping (`mce build`, `mce ship`), that `CameraRail3D.points`, `TimelinePlayer.bindings` and `Blackboard.entities` must be written
  in full, the sky, decal, `BoxCollider3D` and point-light shadow conventions and the three importer rules; `engine_capabilities`
  lists UI documents and scene changes as data. `ui_preview` named the wrong document in its reply (it read a freed asset record
  after the cook); the renderer logs once when decals are dropped past 64 or 3 textures; the `asset_write` refusal and the
  `scene_save` conflict refusal name their way out. A test with two servers on one scene pins that both saves land.
