- Metal: reflection probe captures bind a stand-in for the volumetric fog texture (bank 4), so the Metal debug layer no
  longer aborts the first frame of a scene with a reflection probe ("missing Sampler binding at index 4 for u_volSmplr").
  Found by the hosted macOS Metal workflow (M-METAL-HOSTED); no change on GL, Vulkan, D3D12 or WebGPU output.
