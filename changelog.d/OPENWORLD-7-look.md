- Look pass for the open-world sample, M-OPENWORLD E7: `samples/09_openworld` roads, sidewalks, junctions, parks and ground are textured slabs (tiling colour and normal
  maps from `tools/gen_textures.py`) with road paint (edge and lane lines, double yellow on the avenues, crosswalks, stop lines); trees, lamp posts, benches, hydrants and
  bins are Blender models (`blender/gen_models.py`); cars get headlights that light at dusk; a CC0 thunder clap follows each lightning flash. Data and content only.
- The Ogg Vorbis reader now refuses a comment header whose vendor or comment lengths do not fit the stream (stb_vorbis copied four billion bytes for a length of 0xFFFFFFFF; found by the chunk fuzz test on the new thunder clip).
