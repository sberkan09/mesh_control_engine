- Perceiver forward and `perception_debug_lines`, M-WORKFLOW step 6a (#391): the new MCP tool `perception_debug_lines`
  plays the saved scene headless and reports each `Perceiver`'s world eye, forward, `yaw_deg`, range, angle, what it sees and
  the debug lines (cone edges, arc, line to the target); `kit::perceiver_views` gives C++ the same eye and forward. User guide
  13 now says where the cone points: the world rotation of the entity that holds the `Perceiver`, so `NavAgent3D`
  `face_motion` plus a second system turning a child doubles the yaw.
