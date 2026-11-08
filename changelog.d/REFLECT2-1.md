- SSR roughness blur, M-REFLECT2 steps 48a to 48c: a rough reflecting surface now shows a blurred copy of what the
  screen saw instead of a razor-sharp one. The lit pass gathers 8 taps on a rotated disc of the half-size SSR trace,
  radius `24 * (roughness^2 - 0.01)` half-size texels capped at 8 (`mce/render/ssr_blur.h`, the CPU twin); a missed tap
  lowers the confidence, so a blurred edge fades into the probes, not into black. Roughness at or below 0.1 keeps the
  single sharp tap bit for bit (the `ssr_room` golden is unchanged), as do `r.ssr.blur 0` (new, default 1) and every
  scene with SSR off. Defaults (`ssr_max_roughness` 0.5) are unchanged; a scene that raises it to 0.7 or 0.8 now gets a
  usable rough reflection. `Forward3DStats::ssr_blur` and `ssr_motion` (the latter for step 48e) report what ran.
