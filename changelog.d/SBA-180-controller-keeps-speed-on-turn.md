- Fix (#377): a `CharacterController3D` keeps its speed when only the entity's rotation changes. A `NavAgent3D` with
  `face_motion` turns the entity every tick, which used to zero the controller's velocity and restart its
  `acceleration` ramp each tick, so patrolling sentries crawled; only a moved position resets it now.
