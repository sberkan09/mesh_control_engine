# M-TERRAIN 1 through the stock player, run with cmake -P: a project holding a .mcterrain, a .mcmat and a
# scene with a Terrain3D cooks, and mce_player draws it on the 3D path (the backend MCE_RHI names) with
# zero validation errors, writing a capture.

file(REMOVE_RECURSE "${WORK_DIR}")
file(COPY "${PROJECT_DIR}/" DESTINATION "${WORK_DIR}/project")
execute_process(COMMAND "${MCE_COOK}" "${WORK_DIR}/project" --quiet RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "cook failed (${rc}):\n${out}\n${err}")
endif()
execute_process(COMMAND "${MCE_PLAYER}" -pack "${WORK_DIR}/project/cooked" -frames 20 -capture "${WORK_DIR}/terrain.ppm"
                        -platform.error_box 0 -r.vsync 0 -audio.enabled 0 -pause_on_focus_loss 0
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT "${out}${err}" MATCHES "20 frames, [0-9]+ ticks, 0 GL errors")
    message(FATAL_ERROR "player run failed (${rc}):\n${out}\n${err}")
endif()
if(NOT "${out}${err}" MATCHES "3D last frame ([0-9]+) triangles" OR CMAKE_MATCH_1 LESS 10000)
    message(FATAL_ERROR "the terrain did not draw:\n${out}\n${err}")
endif()
message(STATUS "terrain drawn: ${CMAKE_MATCH_1} triangles")
