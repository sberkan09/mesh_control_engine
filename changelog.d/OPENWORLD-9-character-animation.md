- Characters walk and run in 09_openworld (M-OPENWORLD E9): the player, the police and the crowd play the Blocky Characters'
  idle, walk and sprint clips through one locomotion graph fed by `Locomotion3D`. `Locomotion3D.turn_rate` (with
  `turn_min_speed`) turns a root entity to face where it moves, so an input-driven player faces its walk instead of
  sliding sideways.
