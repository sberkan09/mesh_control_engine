- Render: the sky pass draws `render::Clouds` (`Scene3D::set_clouds()`): a camera-relative cloud plane `height_m`
  above the camera, three octaves of periodic integer-hash value noise with `scale_m` cells thresholded by
  `coverage`, opacity from `density`, drifting along `wind_dir` at `speed` times 10 m/s on the scene clock, lit by
  the frame's sun and sky ambient (sun side brighter, a rim near the disk, a darker base under a thick deck), faded
  into the horizon haze; a cloud in front of the sun dims its disk. New pass uniforms `p_cloud` / `p_cloud2`
  (written only when coverage > 0 and `enabled`; the drift is wrapped in f64 on the CPU, so the clock never costs
  precision). Main pass only (probes, IBL and GI captures stay cloudless). Coverage 0, the Low tier and `r.clouds 0`
  draw byte-identical frames (every existing golden unchanged). New golden `cloud_sky`. (M-CLOUDS 9b)
