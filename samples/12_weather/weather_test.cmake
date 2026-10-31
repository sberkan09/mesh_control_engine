# 12_weather's headless gates (M-WEATHER step 4), run with cmake -P: mce_player plays the cooked pack and the script's
# log lines pin the weather (the script's tick counts from 0 and runs after the weather system, so its line at
# tick N shows the state after N + 1 system ticks; 10 s transitions are 600 ticks). The sun, emitter, lightning and
# environment numbers are in tests/script/test_weather_demo.cpp. Gates:
#   1. 600 ticks: clear, then rain from tick 60 (intensity (N - 60) / 600), the same state hash on two plays;
#   2. 2700 ticks: rain 0.900 at tick 600, storm and snow arrive, clear again at tick 2400, and no problem logged;
#   3. (M-WINDSWAY 2) the "sway:" lines: the factor Kit3DExtract scales the Foliage3D wind amplitude and frequency by,
#      1 + (gain - 1) * Weather.wind with the WindSway gains 4 and 1.5, per weather kind;
#   4. (M-SNOWCOVER 6d) the "snow:" lines: the SnowCover (build 4 s, melt 10 s) is 0 until the snow is in effect (tick
#      1861), builds while it snows (up to tick 2340) and melts after;
#   5. (M-RAINFX 7d) the "rain fx:" lines: the RainRipples strength, RainSplash rate and ThunderDelay speed the script reads;
#   6. (M-WINDDIR 8e) the "wind dir:" lines: the WindDirection heading starts at 0 and turns 6 degrees a minute (0.1 degree a
#      second, so 0.25 degree per 150 ticks);
#   7. (M-CLOUDS 9d) the "clouds:" lines: the CloudLayer authoring fields the script reads;
#   8. (M-WEATHERAUDIO 10c) the "audio:" lines: the rain and wind bed levels the WeatherAudio ramps (3 s fade) toward the
#      weather's targets (the two looping AudioSource entities have no clip, so the sample is silent);
#   9. (M-LIGHTNINGBOLT 11d) the "bolt:" lines: the LightningBolt authoring the script reads and the last strike's place
#      (0 before the first strike at tick 1392, then a hashed point 150..900 m from the camera that stays until the next);
#  10. (M-WINDGUST 12d) the "gust:" lines: the WindGust level (0 in the calm gaps) and the effective wind the wind bed,
#      rain slant and sway read; Weather.wind stays smooth, and the wind bed in the "audio:" lines follows the gusted
#      wind (e.g. 0.371 at tick 450 where the smooth wind is 0.360);
#  11. (M-GROUNDMIST 13c) the "mist:" lines: the clock starts at 5:00 (a 600 s day, 1 h = 1500 ticks), so the GroundMist level
#      starts at 0.185 (a dry dawn on the rise toward the 6:00 peak), falls while the rain and its damping set in, and
#      climbs again as the ground gets wet (0.312 at 5.80 h, tick 1200);
#  12. (M-NIGHTSKY 14f) the "moon:" lines: the Weather entity carries a default Moon (a full moon, 0.5 is the default phase
#      offset, so phase 0.507 on day 0 at 5:00): its path is the sun's, 12 h behind, so it hangs in the west and sets
#      through the dawn. The level starts at 1.000 (full, well up), falls with its height and the rain's damping
#      (0.469 at tick 450, 0.270 at 600 where the storm is called, 0.121 at 1200) and is 0.000 from tick 1500 (below the
#      horizon, the sun is coming up). The Moon is pure data: it changes the state hash (a new column) and nothing else.
#  13. (M-AURORA 16d) the "aurora:" lines: the Aurora on the Weather entity (default gate 55..65 degrees); the script walks
#      the authored latitude from 45 up by 1 degree per 45 ticks with weather.set_aurora, so the gate is closed (level 0.000)
#      until tick 450 (55 degrees), is half open at about 60 degrees and fully open from tick 900 (65 degrees, gate 1.000).
#      The level is the kit's aurora_level on the night fade of the hour (the clock starts at 5:00, so the night fades through
#      the dawn) and the CloudLayer's cover: 0.512 at tick 900, 0.588 at 1050 and falling to 0.000 from tick 2250 (day). The
#      Aurora is pure data here: it changes the state hash (a new column) and nothing else; the entity count stays 14.
#  14. (M-RAINBOW 17d) the "rainbow:" lines: weather.rainbow_level() of the Rainbow on the Weather entity (read only, the script
#      writes nothing, so the state hash is the 17a one). The level is 0.000 by night (the clock starts at 5:00) and while it
#      rains or storms, and rises once the sun is up (tick 1500) over the still wet ground: 0.048 at tick 1800 under the
#      snow, 0.091 at 2250 after the weather is called clear, 0.103 at 2400 (wetness 0.213) and falling again at 2550 as
#      the ground dries below the bow's wetness gate.
#  15. (M-METEOR 18d) the "meteor:" lines: weather.meteor_level() of the Meteor on the Weather entity (read only, the script
#      writes nothing). The clock starts at 5:00 and the night lasts until about 6:00 (tick 1500), so the level is non-zero
#      through the dark hours: 0.422 at tick 0, falling as the dawn gate closes and as the rain (from tick 60) and the storm
#      (from tick 600) thicken the cloud cover (0.030 at 450, 0.002 at 600 in the storm), 0.252 at 1050 once the
#      storm has eased, and exactly 0.000 from sunrise (1500) on, including the clear morning at 2400.
#  16. (M-SEASONS 19c) the "season:" lines: weather.season_phase() of the Season on the Weather entity (read only, the script
#      writes nothing). The scene sets year_days 1 (the WorldClock day is 600 s = 36000 ticks, so a 12-day year would not
#      move in a run): the phase starts at start_phase 0.1 plus 5/24 for the 5:00 start = 0.308 and gains 1/36000 per tick,
#      0.313 at tick 150 and 0.375 (the centre of summer) at tick 2400; the line also logs the authored spring tint;
#  17. (M-LEAFFALL 21c) the "leaves:" lines: weather.leaf_falling() of the LeafFall on the Weather entity (read only, the script
#      writes nothing). The Season only climbs from 0.308 to 0.375 in the run and never reaches the autumn window (0.5 to
#      0.75), so the LeafFall carries manual 0.6, the floor of the amount: every line is 0.600 and the leaf emitter is drawn
#      in all the checked frames. The entity count stays 14.
#  18. (M-FIREFLIES 23c) the "fireflies:" lines: weather.fireflies_glowing() of the Firefly on the Weather entity (read only, the
#      script writes nothing). The amount is max(manual 0.5, the summer window) times the night gate times (1 - rain): the Season
#      only climbs from 0.308 to 0.375 (the window is 0.3 to 0.45, so 1.0 from the start), so it is 1.000 at tick 0 in the
#      dark, falls as the dawn closes the night gate and the rain (from tick 60) and the storm (from 600) thicken (0.850 at tick
#      150, 0.100 at 600 in the storm), recovers to 0.782 at 1050 once the storm has eased, and is exactly 0.000 from sunrise
#      (1500) on. The entity count stays 14.

function(play out_var)
    execute_process(COMMAND "${MCE_PLAYER}" -headless 1 -pack "${PACK}" ${ARGN}
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "mce_player exited ${rc}\n${out}\n${err}")
    endif()
    set(${out_var} "${out}${err}" PARENT_SCOPE)
endfunction()

function(expect text pattern what)
    if(NOT text MATCHES "${pattern}")
        message(FATAL_ERROR "${what} (no match for '${pattern}'):\n${text}")
    endif()
endfunction()

function(expect_not text pattern what)
    if(text MATCHES "${pattern}")
        message(FATAL_ERROR "${what} (unexpected '${pattern}'):\n${text}")
    endif()
endfunction()

function(hash_of text out_var)
    if(NOT text MATCHES "state_hash ([0-9a-f]+)")
        message(FATAL_ERROR "no state hash:\n${text}")
    endif()
    set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# 1. The first 600 ticks.
play(first -ticks 600 -input "${INPUT}")
play(second -ticks 600 -input "${INPUT}")
expect("${first}" "600 ticks, 14 entities, state_hash" "the run did not end after 600 ticks with 14 entities")
expect_not("${first}" "\\[error\\]|\\[warn\\]|does not load|failed at" "the run logged a problem")
expect("${first}" "script: weather: clear intensity 0.000 wind 0.100 wetness 0.000 at tick 0\n" "the weather did not start clear")
expect("${first}" "script: sway: amplitude x1.300 frequency x1.050 at tick 0\n" "the clear sway factor is not 1 + 3 * 0.1")
expect("${first}" "script: sway: amplitude x1.780 frequency x1.130 at tick 300\n" "the rain sway factor did not follow the wind")
expect("${first}" "script: snow: cover 0.000 at tick 450\n" "it snowed cover in the rain")
expect("${first}" "script: rain fx: ripples 1.00 splash 1.00 thunder 343 at tick 450\n" "the rain effects are not wired into the scene")
expect("${first}" "script: wind dir: heading 0.00 drift 6.0 at tick 0\n" "the wind direction did not start at +x")
expect("${first}" "script: wind dir: heading 0.75 drift 6.0 at tick 450\n" "the wind direction did not turn 6 degrees a minute")
expect("${first}" "script: clouds: strength 1.00 fair 0.35 overcast 0.90 height 2000 at tick 450\n" "the cloud layer is not wired into the scene")
expect("${first}" "script: audio: rain 0.000 wind 0.006 max 0/0 dB fade 3 s at tick 0\n" "the audio beds did not start silent")
expect("${first}" "script: audio: rain 0.650 wind 0.371 max 0/0 dB fade 3 s at tick 450\n" "the rain and wind beds did not follow the weather")
expect("${first}" "script: bolt: strike at 0, 0 dist 0 m range 150-900 m height 600 m at tick 450\n" "the bolt is not wired into the scene or struck without a storm")
expect("${first}" "script: gust: level 0.000 wind 0.100 effective 0.100 strength 0.60 at tick 0\n" "the gust did not start calm")
expect("${first}" "script: gust: level 0.053 wind 0.360 effective 0.371 strength 0.60 at tick 450\n" "the gust is not wired into the scene")
expect("${first}" "script: mist: level 0.185 hour 5.00 strength 1.00 peak 6.0 h at tick 0\n" "the mist did not start at the dawn level")
expect("${first}" "script: mist: level 0.114 hour 5.30 strength 1.00 peak 6.0 h at tick 450\n" "the rain did not damp the mist")
expect("${first}" "script: moon: phase 0.507 illumination 1.000 level 1.000 dir 0.95 -0.22 0.23 at tick 0\n" "the moon did not start full and up")
expect("${first}" "script: moon: phase 0.507 illumination 0.999 level 0.469 dir 0.97 -0.15 0.19 at tick 450\n" "the rain did not damp the moon as it sinks")
expect("${first}" "script: aurora: latitude 45.00 gate 0.000 level 0.000 at tick 0\n" "the aurora did not start with a closed gate")
expect("${first}" "script: aurora: latitude 55.00 gate 0.000 level 0.000 at tick 450\n" "the aurora gate is not closed at its low edge")
expect("${first}" "script: rainbow: level 0.000 wetness 0.035 at tick 450\n" "the rainbow shows in the rain before dawn")
expect("${first}" "script: meteor: level 0.422 at tick 0\n" "the meteors did not start at night")
expect("${first}" "script: meteor: level 0.030 at tick 450\n" "the rain did not damp the meteors")
expect("${first}" "script: season: phase 0.308 spring tint 0.75 1.05 0.70 at tick 0\n" "the season did not start at the 5:00 phase")
expect("${first}" "script: season: phase 0.321 spring tint 0.75 1.05 0.70 at tick 450\n" "the season phase did not follow the clock")
expect("${first}" "script: leaves: 0.600 at tick 0\n" "the leaves did not start at the manual floor")
expect("${first}" "script: leaves: 0.600 at tick 450\n" "the leaves did not keep the manual floor")
expect("${first}" "script: fireflies: 1.000 at tick 0\n" "the fireflies did not start at full strength in the dark")
expect("${first}" "script: fireflies: 0.850 at tick 150\n" "the dawn and the rain did not thin the fireflies")
string(REGEX MATCHALL "script: fireflies: [0-9.]+ at tick [0-9]+" fireflies600 "${first}")
list(LENGTH fireflies600 n_fireflies600)
if(NOT n_fireflies600 EQUAL 4)
    message(FATAL_ERROR "expected 4 fireflies lines in 600 ticks, got ${n_fireflies600}")
endif()
string(REGEX MATCHALL "script: leaves: 0.600 at tick [0-9]+" leaves600 "${first}")
list(LENGTH leaves600 n_leaves600)
if(NOT n_leaves600 EQUAL 4)
    message(FATAL_ERROR "expected 4 leaves lines in 600 ticks, got ${n_leaves600}")
endif()
expect("${first}" "script: weather: rain intensity 0.150 wind 0.160 wetness 0.002 at tick 150\n" "rain did not fade in")
expect("${first}" "script: weather: rain intensity 0.400 wind 0.260 wetness 0.013 at tick 300\n" "rain did not follow the 10 s transition")
expect("${first}" "script: weather: rain intensity 0.650 wind 0.360 wetness 0.035 at tick 450\n" "rain did not keep rising")
hash_of("${first}" hash_a)
hash_of("${second}" hash_b)
if(NOT hash_a STREQUAL hash_b)
    message(FATAL_ERROR "two plays differ: ${hash_a} vs ${hash_b}")
endif()

# 2. The whole forecast.
play(forecast -ticks 2700 -input "${INPUT}")
expect("${forecast}" "script: weather: rain intensity 0.900 wind 0.460 wetness 0.068 at tick 600\n" "rain is not at 0.9 when the storm is called")
expect("${forecast}" "script: weather: rain intensity 0.150 wind 0.160 wetness 0.133 at tick 1050\n" "rain did not fade out for the storm")
expect("${forecast}" "script: weather: storm intensity 0.602 wind 0.641 wetness 0.165 at tick 1500\n" "the storm is not at 0.602 when snow is called")
expect("${forecast}" "script: weather: snow intensity 0.400 wind 0.140 wetness 0.209 at tick 2100\n" "snow is not at 0.4 when clear is called")
expect("${forecast}" "script: weather: clear intensity 0.000 wind 0.100 wetness 0.213 at tick 2400\n" "the weather did not clear")
expect("${forecast}" "script: sway: amplitude x2.380 frequency x1.230 at tick 600\n" "the sway factor is wrong when the storm is called")
expect("${forecast}" "script: sway: amplitude x2.924 frequency x1.321 at tick 1500\n" "the storm does not sway the foliage at 2.924 times")
expect("${forecast}" "script: sway: amplitude x1.420 frequency x1.070 at tick 2100\n" "the snow sway factor is wrong")
expect("${forecast}" "script: sway: amplitude x1.300 frequency x1.050 at tick 2400\n" "the sway did not return to the clear factor")
expect("${forecast}" "script: snow: cover 0.000 at tick 1800\n" "snow cover started before the snow")
expect("${forecast}" "script: snow: cover 0.028 at tick 1950\n" "the snow cover did not start building at tick 1861")
expect("${forecast}" "script: snow: cover 0.201 at tick 2100\n" "the snow cover is wrong when clear is called")
expect("${forecast}" "script: snow: cover 0.372 at tick 2250\n" "the snow cover did not keep building")
expect("${forecast}" "script: snow: cover 0.298 at tick 2400\n" "the snow cover did not start to melt")
expect("${forecast}" "script: snow: cover 0.048 at tick 2550\n" "the snow cover did not melt away")
expect("${forecast}" "script: wind dir: heading 4.25 drift 6.0 at tick 2550\n" "the wind direction did not keep turning")
expect("${forecast}" "script: audio: rain 0.602 wind 0.813 max 0/0 dB fade 3 s at tick 1500\n" "the storm beds are wrong")
expect("${forecast}" "script: audio: rain 0.000 wind 0.148 max 0/0 dB fade 3 s at tick 2400\n" "the rain bed did not fall silent in clear weather")
expect("${forecast}" "script: bolt: strike at 0, 0 dist 0 m range 150-900 m height 600 m at tick 1350\n" "a strike had a place before the storm")
expect("${forecast}" "script: bolt: strike at -730, -478 dist 892 m range 150-900 m height 600 m at tick 1500\n" "the first storm strike has the wrong place")
expect("${forecast}" "script: bolt: strike at -312, 720 dist 754 m range 150-900 m height 600 m at tick 2550\n" "the last strike did not stay where it struck")
expect("${forecast}" "script: gust: level 0.062 wind 0.460 effective 0.477 strength 0.60 at tick 600\n" "the gust did not follow the rain")
expect("${forecast}" "script: gust: level 0.493 wind 0.641 effective 0.831 strength 0.60 at tick 1500\n" "the storm gust is wrong")
expect("${forecast}" "script: gust: level 0.807 wind 0.100 effective 0.148 strength 0.60 at tick 2400\n" "a gust did not stir the clear day")
expect("${forecast}" "script: mist: level 0.075 hour 5.40 strength 1.00 peak 6.0 h at tick 600\n" "the mist is wrong when the storm is called")
expect("${forecast}" "script: mist: level 0.312 hour 5.80 strength 1.00 peak 6.0 h at tick 1200\n" "the wet ground did not feed the mist")
expect("${forecast}" "script: mist: level 0.367 hour 6.60 strength 1.00 peak 6.0 h at tick 2400\n" "the mist is wrong at the end of the forecast")
expect("${forecast}" "script: moon: phase 0.508 illumination 0.999 level 0.270 dir 0.98 -0.13 0.18 at tick 600\n" "the moon is wrong when the storm is called")
expect("${forecast}" "script: moon: phase 0.508 illumination 0.999 level 0.121 dir 0.99 -0.05 0.13 at tick 1200\n" "the moon did not keep sinking")
expect("${forecast}" "script: moon: phase 0.508 illumination 0.999 level 0.000 dir 0.99 -0.00 0.10 at tick 1500\n" "the moon did not set by tick 1500")
expect("${forecast}" "script: aurora: latitude 58.33 gate 0.259 level 0.073 at tick 600\n" "the aurora gate did not start to open")
expect("${forecast}" "script: aurora: latitude 61.67 gate 0.741 level 0.295 at tick 750\n" "the aurora gate is wrong halfway")
expect("${forecast}" "script: aurora: latitude 65.00 gate 1.000 level 0.512 at tick 900\n" "the aurora gate is not fully open at its high edge")
expect("${forecast}" "script: aurora: latitude 68.33 gate 1.000 level 0.588 at tick 1050\n" "the aurora level is wrong in the dark")
expect("${forecast}" "script: aurora: latitude 78.33 gate 1.000 level 0.210 at tick 1500\n" "the dawn did not fade the aurora")
expect("${forecast}" "script: aurora: latitude 90.00 gate 1.000 level 0.000 at tick 2400\n" "the aurora shows by day")
expect("${forecast}" "script: rainbow: level 0.000 wetness 0.165 at tick 1500\n" "the rainbow shows at sunrise in the storm")
expect("${forecast}" "script: rainbow: level 0.048 wetness 0.194 at tick 1800\n" "the rainbow did not start with the sun over the wet ground")
expect("${forecast}" "script: rainbow: level 0.091 wetness 0.220 at tick 2250\n" "the rainbow did not rise after the weather cleared")
expect("${forecast}" "script: rainbow: level 0.103 wetness 0.213 at tick 2400\n" "the rainbow is wrong in the clear morning")
expect("${forecast}" "script: rainbow: level 0.064 wetness 0.193 at tick 2550\n" "the rainbow did not fade as the ground dried")
expect("${forecast}" "script: meteor: level 0.002 at tick 600\n" "the storm did not damp the meteors")
expect("${forecast}" "script: meteor: level 0.252 at tick 1050\n" "the meteors are wrong in the dark")
expect("${forecast}" "script: meteor: level 0.000 at tick 1500\n" "the meteors show at sunrise")
expect("${forecast}" "script: meteor: level 0.000 at tick 2400\n" "the meteors show by day")
expect("${forecast}" "script: season: phase 0.350 spring tint 0.75 1.05 0.70 at tick 1500\n" "the season phase is wrong at sunrise")
expect("${forecast}" "script: season: phase 0.375 spring tint 0.75 1.05 0.70 at tick 2400\n" "the season did not reach the centre of summer")
expect("${forecast}" "script: leaves: 0.600 at tick 2550\n" "the leaves did not stay at the manual floor")
string(REGEX MATCHALL "script: leaves: 0.600 at tick [0-9]+" leaves2700 "${forecast}")
list(LENGTH leaves2700 n_leaves2700)
if(NOT n_leaves2700 EQUAL 18)
    message(FATAL_ERROR "expected 18 leaves lines in 2700 ticks, got ${n_leaves2700}")
endif()
expect("${forecast}" "script: fireflies: 0.100 at tick 600\n" "the storm did not damp the fireflies")
expect("${forecast}" "script: fireflies: 0.782 at tick 1050\n" "the fireflies are wrong in the dark after the storm")
expect("${forecast}" "script: fireflies: 0.000 at tick 1500\n" "the fireflies show at sunrise")
expect("${forecast}" "script: fireflies: 0.000 at tick 2400\n" "the fireflies show by day")
string(REGEX MATCHALL "script: fireflies: [0-9.]+ at tick [0-9]+" fireflies2700 "${forecast}")
list(LENGTH fireflies2700 n_fireflies2700)
if(NOT n_fireflies2700 EQUAL 18)
    message(FATAL_ERROR "expected 18 fireflies lines in 2700 ticks, got ${n_fireflies2700}")
endif()
expect_not("${forecast}" "\\[error\\]|\\[warn\\]" "the forecast run logged a problem")

message(STATUS "12_weather holds: rain 0.900 at tick 600, storm 0.602 at 1500, snow 0.400 at 2100, clear at 2400, state_hash ${hash_a} (twice)")
