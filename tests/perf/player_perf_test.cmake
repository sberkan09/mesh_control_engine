# -perf_capture end to end (M-PERF step 1), run with cmake -P: a headless run with a capture prints the
# same state hash as one without, and the .mcperf it writes parses as JSON with the frames, the
# phases and the systems the run had.

function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "exit ${rc}: ${ARGN}\n${out}\n${err}")
    endif()
    set(LAST_OUTPUT "${out}${err}" PARENT_SCOPE)
endfunction()

function(expect_eq actual expected what)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "${what}: expected '${expected}', got '${actual}'")
    endif()
endfunction()

set(SCENE_GUID "6f000000-0000-4000-8000-0000000000e1")
file(REMOVE_RECURSE "${WORK_DIR}")
file(WRITE "${WORK_DIR}/game/assets/scenes/main.mcscene" "{ \"format\": \"mce-scene\", \"version\": 1, \"guid\": \"${SCENE_GUID}\", \"name\": \"main\",
  \"entities\": [ { \"name\": \"Ball\", \"components\": { \"LocalTransform\": { \"position\": [0, 4, 0] },
    \"RigidBody2D\": { \"type\": \"Dynamic\" }, \"Collider2D\": { \"shape\": { \"type\": \"Circle\", \"radius\": 0.5 } } } },
    { \"name\": \"Floor\", \"components\": { \"LocalTransform\": { \"position\": [0, 0, 0] },
    \"RigidBody2D\": {}, \"Collider2D\": { \"shape\": { \"half\": [5, 0.5] }, \"density\": 0 } } } ] }\n")
run("${MCE_COOK}" "${WORK_DIR}/game" --pack --quiet)

set(PLAY "${MCE_PLAYER}" -pack "${WORK_DIR}/game/assets.mce" -scene ${SCENE_GUID} -headless 1 -platform.error_box 0 -ticks 120)
run(${PLAY})
string(REGEX MATCH "state_hash [0-9a-f]+" plain "${LAST_OUTPUT}")
run(${PLAY} -perf_capture "${WORK_DIR}/run.mcperf" -perf_warmup 20)
string(REGEX MATCH "state_hash [0-9a-f]+" captured "${LAST_OUTPUT}")
if(plain STREQUAL "")
    message(FATAL_ERROR "no state hash printed:\n${LAST_OUTPUT}")
endif()
expect_eq("${captured}" "${plain}" "a capture changed the state hash")
if(NOT LAST_OUTPUT MATCHES "perf capture [^\n]*run.mcperf: 100 frames, frame p50")
    message(FATAL_ERROR "no capture summary line:\n${LAST_OUTPUT}")
endif()

file(READ "${WORK_DIR}/run.mcperf" doc)
string(JSON format GET "${doc}" format)
expect_eq("${format}" "mce-perf" "format")
string(JSON version GET "${doc}" version)
expect_eq("${version}" "1" "version")
string(JSON backend GET "${doc}" backend)
expect_eq("${backend}" "headless" "backend")
string(JSON sim_hz GET "${doc}" sim_hz)
expect_eq("${sim_hz}" "60" "sim_hz")
string(JSON frames LENGTH "${doc}" frames)
expect_eq("${frames}" "100" "frames after a 20-frame warm-up")
string(JSON summary_frames GET "${doc}" summary frames)
expect_eq("${summary_frames}" "100" "summary frames")
string(JSON first_tick GET "${doc}" frames 0 tick)
expect_eq("${first_tick}" "21" "the first captured frame follows the warm-up")
string(JSON draws GET "${doc}" summary counters_max draws)
expect_eq("${draws}" "0" "headless frames draw nothing")
string(JSON phase0 GET "${doc}" phases 0)
expect_eq("${phase0}" "Startup" "phase names")
string(JSON render_phase GET "${doc}" render_phases 0)
expect_eq("${render_phase}" "Render_Pre" "render phases")

# The physics step ran once per captured tick, and its time is in the physics phase's column.
string(JSON systems LENGTH "${doc}" systems)
set(physics_runs "")
math(EXPR last "${systems} - 1")
foreach(i RANGE ${last})
    string(JSON phase GET "${doc}" systems ${i} phase)
    string(JSON runs GET "${doc}" systems ${i} runs)
    if(phase STREQUAL "FixedUpdate_Physics" AND runs GREATER 0)
        set(physics_runs "${runs}")
    endif()
endforeach()
expect_eq("${physics_runs}" "100" "physics system runs over the captured frames")

message(STATUS "perf capture holds: state hash unchanged, warm-up skipped, JSON with frames, phases and systems")
