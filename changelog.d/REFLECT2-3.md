- Sample and docs for the second screen-space reflections pass, M-REFLECT2 step 48f. `kit3d_tour` has a `tour_reflect` scene
  (`-scene scenes/tour_reflect.mcscene`, made by `samples/kit3d_tour/gen_reflect.py`): five floor strips from roughness 0.0
  to 0.7 with a pillar behind each, a normal-mapped strip and a mirror strip with a sphere sliding over it, under
  `Environment3D.ssr` with `ssr_max_roughness` 0.8; `-r.quality high` adds TAA, `-r.ssr.blur 0` and `-r.ssr.motion 0` draw it
  without the blur or the velocity read. The user guide (chapter 8.3 cvar table, 8.6 paragraph) documents `r.ssr`,
  `r.ssr.blur` and `r.ssr.motion` and the limits; the milestone is closed.
