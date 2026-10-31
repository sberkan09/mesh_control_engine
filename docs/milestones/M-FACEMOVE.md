# M-FACEMOVE: the player's model faces where it walks (2026-10-31)

Kit: `FaceVelocity3D` (`mce/kit/face_velocity.h`), system `kit_face_velocity` (FixedUpdate_Pre, after `kit3d_bodies`). Put it on a model entity and name the entity that holds the `CharacterController3D` in `source`. While that character moves faster than `min_speed` (0.4 m/s) along the ground, the model's `LocalTransform` rotation turns about Y towards the velocity at `turn_rate_deg` (540 degrees a second), the shortest way round. Standing still keeps the last facing. A seated `Rider3D` source is ignored (the car turns, not the model).

`yaw` is the heading (0 faces -Z, the engine's forward, like crowd and traffic) and `yaw_offset` is added when writing the rotation, for a mesh that faces +Z (the Kenney character: pi). Position and scale are not touched.

Sample (`09_openworld`): the `Player model` child carries it (`source` = Player, offset pi), so the character no longer slides sideways facing the same way; the camera and the gun are unchanged (they follow the camera yaw).

Tests: `tests/kit/test_face_velocity.cpp` (3 cases: heading and keep-on-stop, rate limit, offset). Limits: no walk animation (the blocky rig has no clips), no strafing/aim-facing while the gun is out (the model faces the movement, not the aim), the player's own entity rotation is untouched.
