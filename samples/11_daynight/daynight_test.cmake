# 11_daynight's headless gates (M-DAYNIGHT step 4), run with cmake -P: mce_player plays the cooked pack and the
# script's log lines pin the clock (the script's tick counts from 0 and runs after the clock, so its hour at tick N
# is 5 + (N + 1) / 150). The sun, lamp and window numbers at 06:00, 12:00, 18:00 and 19:30 are in
# tests/script/test_daynight_demo.cpp. Gates:
#   1. the scripted run: Jump at input tick 200 (script tick 199) calls clock.set(12), the hour at tick 300 is
#      12 + 101 / 150, and two plays end on the same state hash;
#   2. without input the hour follows the 60 s day (7.007 at tick 300) and the first midnight (tick 2850) is day 1.

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

# 1. The scripted run.
play(first -ticks 600 -input "${INPUT}")
play(second -ticks 600 -input "${INPUT}")
expect("${first}" "600 ticks, 36 entities, state_hash" "the run did not end after 600 ticks with 36 entities")
expect_not("${first}" "\\[error\\]|\\[warn\\]|does not load|failed at" "the run logged a problem")
expect("${first}" "script: hour 6.007 day 0 at tick 150\n" "the clock did not reach 06:00 after one game hour")
expect("${first}" "script: jump to noon at tick 199\n" "Jump did not reach the script")
expect("${first}" "script: hour 12.673 day 0 at tick 300\n" "clock.set(12) did not move the clock")
expect("${first}" "script: hour 13.673 day 0 at tick 450\n" "the clock stopped after the jump")
hash_of("${first}" hash_a)
hash_of("${second}" hash_b)
if(NOT hash_a STREQUAL hash_b)
    message(FATAL_ERROR "two plays differ: ${hash_a} vs ${hash_b}")
endif()

# 2. A whole day with no input: 2850 ticks from 05:00 to the first midnight.
play(day -ticks 3000)
expect("${day}" "script: hour 7.007 day 0 at tick 300\n" "the clock does not follow the 60 s day")
expect("${day}" "script: hour 18.007 day 0 at tick 1950\n" "the clock is not at 18:00 after 13 game hours")
expect("${day}" "script: hour 0.007 day 1 at tick 2850\n" "midnight did not count the day")
expect_not("${day}" "jump to noon|\\[error\\]|\\[warn\\]" "the quiet run logged a problem")

message(STATUS "11_daynight holds: 06:00 at tick 150, jump to noon 12.673 at tick 300, day 1 at tick 2850, state_hash ${hash_a} (twice)")
