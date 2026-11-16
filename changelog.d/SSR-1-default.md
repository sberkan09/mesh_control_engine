- Screen-space reflections reach rougher surfaces by default (M-REFLECT2 follow-up 4): the default `ssr_max_roughness`
  (`Environment3D` and `render::ScreenSpaceReflections::max_roughness`) is 0.6 instead of 0.5; `ssr_roughness_fade` stays
  0.2. Surfaces up to roughness 0.4 take the full reflection and it fades out by 0.6, so a 0.5 surface now shows a soft
  reflection (the GPU round r8 measured the gather clean there without TAA, for about +0.003 ms). Scenes with SSR on and
  materials between 0.4 and 0.6 (`06_showcase`, `09_openworld`) change look; set `ssr_max_roughness` 0.5 for the old one.
