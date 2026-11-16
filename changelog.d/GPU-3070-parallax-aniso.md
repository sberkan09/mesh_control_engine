- Parallax occlusion mapping looks the same on every GPU: the parallax-shifted material maps were filtered by the sampler's
  anisotropic filter, which each driver implements its own way (an RTX 3070 and llvmpipe differed by 1.8% on the
  `parallax_cobble` golden; llvmpipe alone moved as far between 4x and 16x). The shader now filters those maps
  anisotropically itself (up to 4 trilinear taps along the pixel footprint) and marches the height map at one isotropic
  level. Same look as before at grazing angles; the `parallax_cobble` goldens (linux-gl, linux-sdlgpu) are re-baselined,
  and llvmpipe and lavapipe now agree on it to 0.08% (2.3% before).
