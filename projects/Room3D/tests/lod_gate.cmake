# The mesh LOD gate (#158): the opening view of Room3D (a meadow of 900 rocks cooked with three
# LOD levels) draws at least 5x fewer triangles with LODs on than off, with no GL errors.
#   cmake -DGAME=<room3d executable> -P lod_gate.cmake
foreach(lod 0 1)
    execute_process(COMMAND "${GAME}" -capture_tick 30 -r.lod ${lod} -platform.error_box 0 -r.vsync 0 -audio.enabled 0 -pause_on_focus_loss 0
                    OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0 OR NOT out MATCHES " 0 GL errors")
        message(FATAL_ERROR "room3d -r.lod ${lod} failed (${rc}):\n${out}\n${err}")
    endif()
    if(NOT out MATCHES "3D last frame ([0-9]+) triangles")
        message(FATAL_ERROR "room3d -r.lod ${lod}: no 3D triangle count in:\n${out}")
    endif()
    set(tris_${lod} ${CMAKE_MATCH_1})
endforeach()
math(EXPR need "${tris_1} * 5")
message(STATUS "room3d triangles: ${tris_0} without LODs, ${tris_1} with")
if(need GREATER tris_0)
    message(FATAL_ERROR "LODs cut the far view only from ${tris_0} to ${tris_1} triangles (want at least 5x)")
endif()
