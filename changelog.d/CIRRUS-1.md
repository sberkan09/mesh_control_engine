- M-CIRRUS sky shader (step 53b): the sky now draws `render::Cirrus` as high, thin cirrus streaks on a plane 9 km up,
  combed along `wind_dir` (`stretch` times longer along the wind than across it), with fine curved fibres, soft edges,
  an opacity of at most 0.5 and a fade into the horizon haze. The streaks drift with the scene clock, sit behind the
  low cloud layer, catch silver light near the sun and stay lit and pink after sunset while the low layer has gone
  dark; the water reflects them. `PassU` takes two of its three free `vec4` (`p_cirrus`, `p_cirrus2`, size unchanged).
  The CPU twin of the layer is `mce/render/cirrus.h`. Coverage 0 (the default), `enabled` false, the Low tier and
  `r.cirrus 0` draw every frame exactly as before; the IBL, reflection probes and cloud shadows do not see the layer.
