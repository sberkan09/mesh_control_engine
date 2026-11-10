# A soak gate (M-SOAK step 1), run with cmake -P: a headless run of 1500 ticks must not leak. The live
# heap allocation count the capture records each frame is the same in the last frame as it was
# after the warm-up (plus a small slack for caches that fill once), so a per-tick allocation that is
# never freed shows here long before it shows as memory.

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
    \"RigidBody2D\": { \"type\": \"Dynamic\" }, \"Collider2D\": { \"shape\": { \"type\": \"Circle\", \"radius\": 0.5 } } } },
    { \"name\": \"Floor\", \"components\": { \"LocalTransform\": { \"position\": [0, 0, 0] },
    \"RigidBody2D\": {}, \"Collider2D\": { \"shape\": { \"half\": [5, 0.5] }, \"density\": 0 } } } ] }\n")
run("${MCE_COOK}" "${WORK_DIR}/game" --pack --quiet)

run("${MCE_PLAYER}" -pack "${WORK_DIR}/game/assets.mce" -scene ${SCENE_GUID} -headless 1 -platform.error_box 0
    -ticks 1500 -perf_capture "${WORK_DIR}/soak.mcperf" -perf_warmup 100)
file(READ "${WORK_DIR}/soak.mcperf" doc)
string(JSON frames LENGTH "${doc}" frames)
math(EXPR last "${frames} - 1")
string(JSON first_live GET "${doc}" frames 0 counters heap_allocations)
string(JSON last_live GET "${doc}" frames ${last} counters heap_allocations)
message(STATUS "live heap allocations: ${first_live} after the warm-up, ${last_live} after ${frames} more frames")
math(EXPR limit "${first_live} + ${SLACK}")
if(last_live GREATER limit)
    message(FATAL_ERROR "the live heap allocation count grew from ${first_live} to ${last_live} over ${frames} frames (slack ${SLACK})")
endif()
