- Wind gusts, M-WINDGUST step 12c: `kit_precipitation` (rain and snow slant) and `kit_weather_audio` (wind bed target)
  read `effective_wind` with the rain and audio bits. Both already run after `kit_wind_gust` (registration order), so a
  gust arrives in the same tick. The wind fed to the emitter is rounded to 1/64 only while a gust applies; without a
  `WindGust` (or with the bit clear, `enabled` 0 or a zero level) emitters and gains are bit-identical. Guide 9.19h.
