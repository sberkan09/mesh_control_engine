# 13_water's headless gates (M-WATER step 15d), run with cmake -P: mce_player plays the cooked pack and the script's log
# lines pin the water (the script's tick counts from 0; the scene time at its tick N is N * dt). Gates:
#   1. 600 ticks: 7 entities, no problem logged, the same state hash on two plays, 4 `water:` lines;
#   2. every `water:` line: the crate's y is the surface height + 0.1 (it follows water.height_at), the height is within
#      the largest the four default waves can sum to (0.4 m * (1 + 0.6 + 0.4 + 0.27) = 0.908 m), the surface normal is
#      near +y, a point 0.3 m under the surface is submerged and one 0.3 m over it is not;
#   3. the surface moves: the lines show many different heights;
#   4. 2700 ticks: the same bounds on all 18 lines.

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

# "-0.520" -> -520 (the script prints three decimals; leading zeros are stripped so math() does not read octal).
function(milli value out_var)
    string(REPLACE "." "" digits "${value}")
    set(sign "")
    if(digits MATCHES "^-")
        set(sign "-")
        string(SUBSTRING "${digits}" 1 -1 digits)
    endif()
    string(REGEX REPLACE "^0+" "" digits "${digits}")
    if(digits STREQUAL "")
        set(digits 0)
    endif()
    set(${out_var} "${sign}${digits}" PARENT_SCOPE)
endfunction()

# Checks every `water:` line of `text`; sets ${count_var} (the lines) and ${distinct_var} (the different heights).
function(check_water_lines text count_var distinct_var)
    string(REGEX MATCHALL "script: water: height [-0-9.]+ crate [-0-9.]+ tilt [-0-9.]+ submerged [a-z]+ above [a-z]+ at tick [0-9]+"
           lines "${text}")
    set(seen "")
    foreach(line IN LISTS lines)
        string(REGEX MATCH "height ([-0-9.]+) crate ([-0-9.]+) tilt ([-0-9.]+) submerged ([a-z]+) above ([a-z]+) at tick ([0-9]+)" m "${line}")
        set(h "${CMAKE_MATCH_1}")
        set(c "${CMAKE_MATCH_2}")
        set(tilt "${CMAKE_MATCH_3}")
        set(under "${CMAKE_MATCH_4}")
        set(over "${CMAKE_MATCH_5}")
        set(tick "${CMAKE_MATCH_6}")
        milli("${h}" hm)
        milli("${c}" cm)
        math(EXPR gap "${cm} - ${hm}")
        # both are printed to three decimals: a rounding step either way (98..102)
        if(gap LESS 98 OR gap GREATER 102)
            message(FATAL_ERROR "the crate does not follow the surface (height ${h}, crate ${c}) at tick ${tick}")
        endif()
        if(hm GREATER 908 OR hm LESS -908)
            message(FATAL_ERROR "the surface leaves the wave amplitude (height ${h}) at tick ${tick}")
        endif()
        if(NOT under STREQUAL "true" OR NOT over STREQUAL "false")
            message(FATAL_ERROR "submerged is wrong 0.3 m under (${under}) or over (${over}) the surface at tick ${tick}")
        endif()
        milli("${tilt}" tm)
        if(tm LESS 900)
            message(FATAL_ERROR "the surface normal is not near +y (${tilt}) at tick ${tick}")
        endif()
        list(APPEND seen "${h}")
    endforeach()
    list(LENGTH lines n)
    list(REMOVE_DUPLICATES seen)
    list(LENGTH seen d)
    set(${count_var} ${n} PARENT_SCOPE)
    set(${distinct_var} ${d} PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# 1. The first 600 ticks.
play(first -ticks 600 -input "${INPUT}")
play(second -ticks 600 -input "${INPUT}")
expect("${first}" "600 ticks, 7 entities, state_hash" "the run did not end after 600 ticks with 7 entities")
expect_not("${first}" "\\[error\\]|\\[warn\\]|does not load|failed at" "the run logged a problem")
expect("${first}" "script: water: height 0.115 crate 0.215 tilt 0.989 submerged true above false at tick 0\n" "the crate did not start on the surface")
expect("${first}" "script: water: height -0.520 crate -0.420 tilt 0.992 submerged true above false at tick 150\n" "the crate did not follow the trough at 2.5 s")
expect("${first}" "script: water: height 0.313 crate 0.413 tilt 0.975 submerged true above false at tick 300\n" "the crate did not follow the crest at 5 s")
expect("${first}" "script: water: height 0.382 crate 0.482 tilt 0.966 submerged true above false at tick 450\n" "the surface at 7.5 s is not the pinned sum of the waves")
hash_of("${first}" hash_a)
hash_of("${second}" hash_b)
if(NOT hash_a STREQUAL hash_b)
    message(FATAL_ERROR "two plays differ: ${hash_a} vs ${hash_b}")
endif()
check_water_lines("${first}" n600 d600)
if(NOT n600 EQUAL 4 OR d600 LESS 4)
    message(FATAL_ERROR "expected 4 water lines with a moving surface, got ${n600} lines and ${d600} different heights")
endif()

# 2. A longer run.
play(longer -ticks 2700 -input "${INPUT}")
expect_not("${longer}" "\\[error\\]|\\[warn\\]" "the long run logged a problem")
hash_of("${longer}" hash_c)
check_water_lines("${longer}" n2700 d2700)
if(NOT n2700 EQUAL 18 OR d2700 LESS 15)
    message(FATAL_ERROR "expected 18 water lines with a moving surface, got ${n2700} lines and ${d2700} different heights")
endif()

message(STATUS "13_water holds: the crate rides 0.1 m over the surface within 0.908 m on ${n2700} lines, state_hash ${hash_a} at 600 ticks (twice), ${hash_c} at 2700")
