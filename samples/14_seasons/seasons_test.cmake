# 14_seasons' headless gate (M-SEASONTOUR step 22b), run with cmake -P: mce_player plays the cooked pack for the whole year
# (3600 ticks at 60 Hz: a 2 s WorldClock day, a 30-day Season, so the phase is tick / 3600) and the script's `season:` lines
# pin the four season centres (the script's tick N shows the state after N + 1 system ticks; values are what a real run printed):
#   spring  tick  450: phase 0.125, frozen 0.000, leaves 0.000
#   summer  tick 1350: phase 0.375, frozen 0.000, leaves 0.000
#   autumn  tick 2250: phase 0.625, frozen 0.000, leaves 1.000
#   winter  tick 3150: phase 0.875, frozen 1.000, leaves 0.000
# plus: 24 lines (ticks 0 to 3450), leaves non-zero only between ticks 1800 and 2700 (1950 to 2550 are the lines), the freeze
# non-zero only after tick 2400 (2550 to 3300, thawed by 3450), the phase rising by tick, no problem logged, 20 entities,
# and the same state hash on two plays. The camera orbit is not asserted beyond the hash.

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


file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

play(first -ticks 3600 -input "${INPUT}")
play(second -ticks 3600 -input "${INPUT}")
expect("${first}" "3600 ticks, 20 entities, state_hash" "the run did not end after 3600 ticks with 20 entities")
expect_not("${first}" "\\[error\\]|\\[warn\\]|does not load|failed at" "the run logged a problem")
hash_of("${first}" hash_a)
hash_of("${second}" hash_b)
if(NOT hash_a STREQUAL hash_b)
    message(FATAL_ERROR "two plays differ: ${hash_a} vs ${hash_b}")
endif()

# The four season centres.
expect("${first}" "script: season: phase 0.125 frozen 0.000 leaves 0.000 at tick 450\n" "spring is not the pinned state")
expect("${first}" "script: season: phase 0.375 frozen 0.000 leaves 0.000 at tick 1350\n" "summer is not the pinned state")
expect("${first}" "script: season: phase 0.625 frozen 0.000 leaves 1.000 at tick 2250\n" "autumn does not drop leaves over an open lake")
expect("${first}" "script: season: phase 0.875 frozen 1.000 leaves 0.000 at tick 3150\n" "winter does not freeze the lake with no leaves")

# All 24 lines: the phase climbs, the leaves are non-zero only in 1800..2700, the ice only after 2400 and gone by 3450.
string(REGEX MATCHALL "script: season: phase [0-9.]+ frozen [0-9.]+ leaves [0-9.]+ at tick [0-9]+" lines "${first}")
list(LENGTH lines n)
if(NOT n EQUAL 24)
    message(FATAL_ERROR "expected 24 season lines, got ${n}")
endif()
set(last_phase -1)
set(n_leaves 0)
set(n_frozen 0)
foreach(line IN LISTS lines)
    string(REGEX MATCH "phase ([0-9.]+) frozen ([0-9.]+) leaves ([0-9.]+) at tick ([0-9]+)" m "${line}")
    milli("${CMAKE_MATCH_1}" phase)
    milli("${CMAKE_MATCH_2}" frozen)
    milli("${CMAKE_MATCH_3}" leaves)
    set(tick "${CMAKE_MATCH_4}")
    if(NOT phase GREATER last_phase)
        message(FATAL_ERROR "the season phase does not climb at tick ${tick}")
    endif()
    set(last_phase ${phase})
    if(frozen GREATER 1000 OR leaves GREATER 1000)
        message(FATAL_ERROR "an amount left 0..1 at tick ${tick}")
    endif()
    if(leaves GREATER 0)
        math(EXPR n_leaves "${n_leaves} + 1")
        if(tick LESS 1800 OR tick GREATER 2700)
            message(FATAL_ERROR "leaves fall outside autumn (tick ${tick})")
        endif()
    endif()
    if(frozen GREATER 0)
        math(EXPR n_frozen "${n_frozen} + 1")
        if(tick LESS 2400 OR tick GREATER 3300)
            message(FATAL_ERROR "the lake is frozen outside winter (tick ${tick})")
        endif()
    endif()
endforeach()
if(n_leaves LESS 5 OR n_frozen LESS 5)
    message(FATAL_ERROR "the leaves (${n_leaves} lines) or the ice (${n_frozen} lines) never show")
endif()
expect("${first}" "frozen 0.000 leaves 0.000 at tick 3450\n" "the lake did not thaw by the end of the year")

message(STATUS "14_seasons holds: the four season centres, ${n_leaves} lines with leaves and ${n_frozen} with ice, state_hash ${hash_a} (twice)")
