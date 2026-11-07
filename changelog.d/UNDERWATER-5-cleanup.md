- M-UNDERWATER cleanup (UNDERWATER-5): the underwater pipeline is now created at startup in `create_pipelines` (like the lens
  rain) instead of on the first dive, so a refused shader is per renderer rather than per process and the first dive has no
  compile hitch; the off path still records and creates nothing. Stale `record_underwater` comment in `forward3d.h` fixed
  (comment-only change to a wide header, include fan-out unchanged at 183 TUs). No frame changes.
