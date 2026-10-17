- 3D movement demo, M-MOVE3D step 6 (SBA-139): `samples/08_movement`, built over MCP alone, has a humanoid run an obstacle
  course (a hurdle that needs a jump, a lintel that needs a crouch, a timeline-driven lift up to a ledge) on a
  `CharacterController3D` with acceleration, coyote time and crouch, animated by `Locomotion3D` under a spring-arm
  `CameraFollow3D`, while three karts race two laps on `VehicleDriver3D` and `Waypoint3D` points. `ctest -R sample_08_movement`
  plays it, the determinism stage hashes it in Debug and Release, and a kit test runs 64 characters and 8 driven cars together
  (6.4 ms a tick in Debug). User guide 18 (samples) and 14 (MCP workflow) describe it. M-MOVE3D is done.
