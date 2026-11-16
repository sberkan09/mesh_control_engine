# 09_openworld's headless contract (cmake -P): the pack plays with no error and two plays of the same ticks end on
# the same state hash. Like samples/tour_test.cmake, but the entity count is free: the traffic and crowd spawners
# add and remove entities as the player moves, so it is only printed.
function(run_player out_var)
    execute_process(COMMAND "${MCE_PLAYER}" -headless 1 -ticks "${TICKS}" -pack "${PACK}" -scene "${SCENE}" -input "${INPUT}"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "mce_player exited ${rc}\n${out}\n${err}")
    endif()
    if("${out}${err}" MATCHES "does not load|\\[error\\]|\\[warn\\]")
        message(FATAL_ERROR "the city logged a problem:\n${out}\n${err}")
    endif()
    if(NOT out MATCHES "${TICKS} ticks, ([0-9]+) entities, state_hash ([0-9a-f]+)")
        message(FATAL_ERROR "expected ${TICKS} ticks with a state hash:\n${out}\n${err}")
    endif()
    set(${out_var} "${CMAKE_MATCH_2}" PARENT_SCOPE)
    set(${out_var}_entities "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

run_player(first)
run_player(second)
if(NOT first STREQUAL second)
    message(FATAL_ERROR "two plays differ: ${first} vs ${second}")
endif()
message(STATUS "${TOUR}: ${TICKS} ticks, ${first_entities} entities, state_hash ${first} (twice)")
