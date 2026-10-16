# ai_demo headless contract (cmake -P): the stock player runs the trees from the scene. The thief
# sneaks in at 3 s and makes a noise; the guard hears it, leaves its patrol to look, spots the thief
# with its Perceiver and catches it. Two plays hash the same.

function(run_player out_var)
    execute_process(COMMAND "${MCE_PLAYER}" -headless 1 -ticks 1200 -pack "${PACK}"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "mce_player exited ${rc}\n${out}\n${err}")
    endif()
    set(all "${out}${err}")
    if(all MATCHES "does not load|cannot be loaded|is not registered|\\[error\\]")
        message(FATAL_ERROR "the demo logged an error:\n${all}")
    endif()
    foreach(line "thief \\(entity [0-9]+\\): sneaking in" "guard \\(entity [0-9]+\\): heard something"
                 "guard \\(entity [0-9]+\\): spotted the thief" "guard \\(entity [0-9]+\\): caught the thief")
        if(NOT all MATCHES "${line}")
            message(FATAL_ERROR "no '${line}' in the log:\n${all}")
        endif()
    endforeach()
    if(NOT out MATCHES "1200 ticks, [0-9]+ entities, state_hash ([0-9a-f]+)")
        message(FATAL_ERROR "no state hash:\n${all}")
    endif()
    set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

run_player(first)
run_player(second)
if(NOT first STREQUAL second)
    message(FATAL_ERROR "two plays differ: ${first} vs ${second}")
endif()
message(STATUS "ai_demo: the guard caught the thief (state_hash ${first})")
