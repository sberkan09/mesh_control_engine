# The editor's Profiler opens a capture (M-PERF step 4), run with cmake -P: the player records a
# headless .mcperf, and the editor, started with -editor.perf_capture, draws it in the Profiler's
# Capture tab (frame strip, frame breakdown, systems) for a few frames and exits cleanly.

function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "exit ${rc}: ${ARGN}\n${out}\n${err}")
    endif()
    set(LAST_OUTPUT "${out}${err}" PARENT_SCOPE)
endfunction()

set(SCENE_GUID "6f000000-0000-4000-8000-0000000000e2")
file(REMOVE_RECURSE "${WORK_DIR}")
file(WRITE "${WORK_DIR}/game/assets/scenes/main.mcscene" "{ \"format\": \"mce-scene\", \"version\": 1, \"guid\": \"${SCENE_GUID}\", \"name\": \"main\",
  \"entities\": [ { \"name\": \"Ball\", \"components\": { \"LocalTransform\": { \"position\": [0, 4, 0] },
    \"RigidBody2D\": { \"type\": \"Dynamic\" }, \"Collider2D\": { \"shape\": { \"type\": \"Circle\", \"radius\": 0.5 } } } } ] }\n")
run("${MCE_COOK}" "${WORK_DIR}/game" --pack --quiet)
run("${MCE_PLAYER}" -pack "${WORK_DIR}/game/assets.mce" -scene ${SCENE_GUID} -headless 1 -platform.error_box 0 -ticks 60
    -perf_capture "${WORK_DIR}/run.mcperf" -perf_warmup 10)

run("${MCE_EDITOR}" -frames 20 -editor.ini "" -platform.error_box 0 -r.vsync 0 -editor.perf_capture "${WORK_DIR}/run.mcperf")
if(NOT LAST_OUTPUT MATCHES "Profiler: opened [^\n]*run.mcperf \\(50 frames, [1-9][0-9]* systems")
    message(FATAL_ERROR "the Profiler did not open the capture:\n${LAST_OUTPUT}")
endif()

# A file that is not a capture is reported, not fatal.
file(WRITE "${WORK_DIR}/bad.mcperf" "{ \"format\": \"something else\" }\n")
run("${MCE_EDITOR}" -frames 10 -editor.ini "" -platform.error_box 0 -r.vsync 0 -editor.perf_capture "${WORK_DIR}/bad.mcperf")
if(NOT LAST_OUTPUT MATCHES "Profiler: [^\n]*bad.mcperf: ")
    message(FATAL_ERROR "no error for a file that is not a capture:\n${LAST_OUTPUT}")
endif()
