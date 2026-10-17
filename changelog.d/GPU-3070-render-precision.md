- Rendering fixes from the first real-GPU run (RTX 3070): film grain picks its cell in integers (with the default 1.5 px cell,
  every third row and column sat on a cell edge and each driver rounded it its own way); parallax-shifted materials sample
  their maps with the mesh uv's gradients instead of the stepped uv's own (which differ between coarse and fine derivatives);
  and a scene that turns on a GI volume grows the shadow map before the probe captures bind it, not after (they sampled a
  deleted texture with a depth-compare sampler on that frame; NVIDIA warned of undefined behavior). Goldens refreshed:
  `filmic_lens` (GL, SDL GPU) and `parallax_cobble` (SDL GPU).
