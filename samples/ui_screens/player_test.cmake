# ui_screens headless contract (cmake -P): the stock player loads the scene's UI screens with no
# host code, gameplay reacts to the actions the pause menu names, and replays hold their hash.

function(run_player out_var)
    execute_process(COMMAND "${MCE_PLAYER}" -headless 1 -ticks 240 -pack "${PACK}" -module "${MODULE}" ${ARGN}
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "mce_player exited ${rc}: ${ARGN}\n${out}\n${err}")
    endif()
    if(NOT "${out}${err}" MATCHES "2 UI screen\\(s\\), 3 action\\(s\\)")
        message(FATAL_ERROR "the UI screens did not load:\n${out}\n${err}")
    endif()
    if(NOT out MATCHES "240 ticks, [0-9]+ entities, state_hash ([0-9a-f]+)")
        message(FATAL_ERROR "no state hash:\n${out}\n${err}")
    endif()
    set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
file(WRITE "${WORK_DIR}/pause.txt" "# the HUD's pause button, then the menu's resume button\n60 Pause\n120 Resume\n")

run_player(plain)
run_player(paused -input "${WORK_DIR}/pause.txt")
run_player(paused_again -input "${WORK_DIR}/pause.txt")
if(plain STREQUAL paused)
    message(FATAL_ERROR "the pause menu changed nothing (hash ${plain})")
endif()
if(NOT paused STREQUAL paused_again)
    message(FATAL_ERROR "the replay does not hold its hash: ${paused} vs ${paused_again}")
endif()
message(STATUS "ui_screens: plain ${plain}, paused ${paused} (replayed)")
