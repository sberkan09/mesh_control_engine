# M-PAINTWEAR — worn road markings

Owner: rutin 2 (M-VISUAL), SBA-237 (new milestone, started by the coordinator's request; Linear has no open M-VISUAL work).
Follow-up of [M-ROADWEAR](M-ROADWEAR.md).

**Problem.** Lane lines, crosswalks and stop lines in `samples/09_openworld` are flat-colour boxes: perfectly clean paint on a
worn road.

**Plan.**
1. (this PR, data only) A tiling `paintwear_color.jpg` multiplies the marking colour: duller tyre-worn zones and small chips where
   the asphalt shows through. The two marking materials get it as `base_color`; the paint meshes get world-metre UVs (3 m tile).
2. (later, needs the coordinator because it touches the material path) Let the paint show the road through the chips with an
   alpha-cutout or a roughness map so worn paint also loses its sheen in the wet.
3. (later) Reflective-paint behaviour at night under headlights.

**Check.** Junction frame after step 1: `/mnt/project-files/notes/paintwear-after.png` (crosswalk and lane lines, zoomed).
Only data files change; no code, component or shader.
