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
#      wind (e.g. 0.371 at tick 450 where the smooth wind is 0.360).

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
expect_not("${forecast}" "\\[error\\]|\\[warn\\]" "the forecast run logged a problem")

message(STATUS "12_weather holds: rain 0.900 at tick 600, storm 0.602 at 1500, snow 0.400 at 2100, clear at 2400, state_hash ${hash_a} (twice)")
