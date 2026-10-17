- World transforms reach every `FixedUpdate_Post` system on the same tick, M-MOVE3D step 6b (SBA-139, #344): the hierarchy
  now propagates `WorldTransform` at the front of the phase as well as where it was installed, so the kit's animators, foot IK,
  bone attachments and follow cameras no longer read last tick's pose when a scene installs the hierarchy after the kit (every
  shipped player). `World::add_system_first` and `World::add_tick_clamp_hook` are new; the hierarchy's run ticks age with the
  World's, so a move after a very long run is not lost. Replay hashes: only `07_gameplay3d` changes, its camera now follows the
  cart's current pose.
