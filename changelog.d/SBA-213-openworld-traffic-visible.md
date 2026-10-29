- 09_openworld traffic and walkers on screen (SBA-213): pedestrians with a kinematic `RigidBody3D` walk again. The crowd
  runs after the body sync, stands such a walker on its capsule (its origin is the body's centre) and drives the body by
  velocity, so contacts see a walking speed and far walkers glide between steps; a walker turns round at a parked car
  instead of shoving it. Before, the physics write-back put them back on their spawn point every tick, so they never moved
  and never visibly fled. `samples/09_openworld` starts its camera looking along the avenue, so the traffic and the crowd
  are in the first frame; `drive.input` walks forward to the car.
