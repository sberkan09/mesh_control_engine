- Metal: reflection probe and irradiance-volume captures bind stand-ins for the volumetric fog texture (bank 4) and the decal
  textures (banks 0 to 2), so the Metal debug layer no longer aborts the first frame of a scene with a probe or a GI volume
  ("missing Sampler binding at index 4 for u_volSmplr"). Found by the hosted macOS Metal workflow (M-METAL-HOSTED); no change
  to any backend's output.
