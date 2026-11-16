- Rain and wind ambience beds, M-WEATHERAUDIO step 10a: the optional `WeatherAudio` component (`rain_source`,
  `wind_source`, `rain_db`, `wind_db`, `fade_seconds` 3, `enabled`) and the system `kit_weather_audio` (after
  `kit_weather`) drive `AudioSource::gain_db` of two author-placed looping sources from `Weather`: rain level = kind
  weight (clear 0, overcast 0.15, rain/storm 1, snow 0.35) x intensity, wind level = `Weather.wind`, each a saved
  integer ramp (`rain_q`, `wind_q` out of 2^24), gain = max dB + 20 log10(level) floored at -60. Sources are never
  started or stopped and `Weather` is not written; a world without `Weather`, `WeatherAudio` or audio resources is
  untouched. Guide 9.19f.
