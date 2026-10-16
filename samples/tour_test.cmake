# The tour samples' headless contract (cmake -P): the pack plays with no error and two plays of the
# same ticks end on the same state hash (the M-COVER samples are data, so this is the whole test).
function(run_player out_var)
    execute_process(COMMAND "${MCE_PLAYER}" -headless 1 -ticks "${TICKS}" -pack "${PACK}"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "mce_player exited ${rc}\n${out}\n${err}")
    endif()
    if("${out}${err}" MATCHES "does not load|\\[error\\]|\\[warn\\]")
        message(FATAL_ERROR "the tour logged a problem:\n${out}\n${err}")
    endif()
    if(NOT out MATCHES "${TICKS} ticks, ${ENTITIES} entities, state_hash ([0-9a-f]+)")
        message(FATAL_ERROR "expected ${TICKS} ticks and ${ENTITIES} entities with a state hash:\n${out}\n${err}")
    endif()
    set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

run_player(first)
run_player(second)
if(NOT first STREQUAL second)
    message(FATAL_ERROR "two plays differ: ${first} vs ${second}")
endif()
message(STATUS "${TOUR}: ${TICKS} ticks, state_hash ${first} (twice)")
