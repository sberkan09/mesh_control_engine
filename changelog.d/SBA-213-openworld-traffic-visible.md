- 09_openworld traffic and walkers on screen (SBA-213): pedestrians with a kinematic `RigidBody3D` walk again (the crowd
  moves the body with the walker; before, the physics write-back put them back on their spawn point every tick, so they
  never moved and never visibly fled). `samples/09_openworld` starts its camera looking along the avenue, so the traffic
  and the crowd are in the first frame; the walker prefab stands on the pavement instead of half under it, and the parked
  cars sit off the walkers' line. `drive.input` walks forward to the car.
