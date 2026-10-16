# cinema_demo headless contract (cmake -P): the intro cutscene plays through, holding the skip
# action skips it (the run changes), and a replayed input holds its hash.

function(run_player out_var)
    execute_process(COMMAND "${MCE_PLAYER}" -headless 1 -ticks 780 -pack "${PACK}" ${ARGN}
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "mce_player exited ${rc}: ${ARGN}\n${out}\n${err}")
    endif()
    if("${out}${err}" MATCHES "does not load|\\[error\\]")
        message(FATAL_ERROR "the demo logged an error:\n${out}\n${err}")
    endif()
    if(NOT out MATCHES "780 ticks, [0-9]+ entities, state_hash ([0-9a-f]+)")
        message(FATAL_ERROR "no state hash:\n${out}\n${err}")
    endif()
    set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
file(WRITE "${WORK_DIR}/skip.txt" "# hold the skip action for 80 ticks, mid-cutscene\n300-380 Cancel\n")

run_player(plain)
run_player(plain_again)
if(NOT plain STREQUAL plain_again)
    message(FATAL_ERROR "two plays of the cutscene differ: ${plain} vs ${plain_again}")
endif()
run_player(skipped -input "${WORK_DIR}/skip.txt")
if(plain STREQUAL skipped)
    message(FATAL_ERROR "holding the skip action changed nothing (hash ${plain})")
endif()
# The same input again (the stock player replays a scripted input, not a recording; the
# recorded-stream replay is tests/kit/test_cinematic.cpp's).
run_player(replayed -input "${WORK_DIR}/skip.txt")
if(NOT skipped STREQUAL replayed)
    message(FATAL_ERROR "the replay does not hold its hash: ${skipped} vs ${replayed}")
endif()
message(STATUS "cinema_demo: played ${plain}, skipped ${skipped} (replayed)")
