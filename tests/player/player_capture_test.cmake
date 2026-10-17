# The player's capture without a display and its save folder line (M-WORKFLOW step 4, GitHub #380), run
# with cmake -P.
#   MODE=save     headless: "save folder <dir>" is logged once at start; $XDG_DATA_HOME moves the default
#                 (Linux), -save_dir wins over both.
#   MODE=capture  windowed with DISPLAY, WAYLAND_DISPLAY, SDL_VIDEODRIVER and SDL_VIDEO_DRIVER unset: -capture_tick still
#                 writes a PPM of the window's size on the frame after that tick (the player falls back to the offscreen
#                 driver and says so), while an explicit -platform.video_driver is not second-guessed.

function(run_env expect_rc)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL expect_rc)
        message(FATAL_ERROR "exit ${rc}, wanted ${expect_rc}: ${ARGN}\n${out}\n${err}")
    endif()
    set(LAST_OUTPUT "${out}${err}" PARENT_SCOPE)
endfunction()

set(SCENE_GUID "6f000000-0000-4000-8000-0000000000e2")
file(REMOVE_RECURSE "${WORK_DIR}")
file(WRITE "${WORK_DIR}/game/assets/scenes/main.mcscene" "{ \"format\": \"mce-scene\", \"version\": 1, \"guid\": \"${SCENE_GUID}\", \"name\": \"main\",
  \"entities\": [ { \"name\": \"Ball\", \"components\": { \"LocalTransform\": { \"position\": [0, 4, 0] },
    \"RigidBody2D\": { \"type\": \"Dynamic\" }, \"Collider2D\": { \"shape\": { \"type\": \"Circle\", \"radius\": 0.5 } } } } ] }\n")
execute_process(COMMAND "${MCE_COOK}" "${WORK_DIR}/game" --pack --quiet RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "cook failed: ${out}${err}")
endif()
set(PACK "${WORK_DIR}/game/assets.mce")
set(COMMON -pack "${PACK}" -scene ${SCENE_GUID} -platform.error_box 0)
# SDL 3 reads both SDL_VIDEO_DRIVER and SDL_VIDEODRIVER; either one set to "offscreen" skips the fallback.
set(NOENV ${CMAKE_COMMAND} -E env --unset=DISPLAY --unset=WAYLAND_DISPLAY --unset=SDL_VIDEODRIVER --unset=SDL_VIDEO_DRIVER)

if(MODE STREQUAL "save")
    if(UNIX AND NOT APPLE)
        run_env(0 ${CMAKE_COMMAND} -E env "XDG_DATA_HOME=${WORK_DIR}/xdg" ${MCE_PLAYER} ${COMMON} -headless 1 -ticks 5)
        string(REGEX MATCHALL "save folder [^\n]*" lines "${LAST_OUTPUT}")
        list(LENGTH lines n)
        if(NOT n EQUAL 1)
            message(FATAL_ERROR "the save folder is logged once, got ${n}:\n${LAST_OUTPUT}")
        endif()
        if(NOT LAST_OUTPUT MATCHES "save folder ${WORK_DIR}/xdg/MCE/MCE/saves")
            message(FATAL_ERROR "XDG_DATA_HOME did not move the save folder:\n${LAST_OUTPUT}")
        endif()
    endif()
    run_env(0 ${MCE_PLAYER} ${COMMON} -headless 1 -ticks 5 -save_dir "${WORK_DIR}/mysaves")
    if(NOT LAST_OUTPUT MATCHES "save folder ${WORK_DIR}/mysaves")
        message(FATAL_ERROR "-save_dir is not the logged save folder:\n${LAST_OUTPUT}")
    endif()
    message(STATUS "save folder: logged once, XDG_DATA_HOME moves the default, -save_dir wins")
elseif(MODE STREQUAL "capture")
    set(SHOT "${WORK_DIR}/shots/a.ppm")
    run_env(0 ${NOENV} ${MCE_PLAYER} ${COMMON} -window 320x180 -r.vsync 0 -capture_tick 30 -capture "${SHOT}")
    if(NOT LAST_OUTPUT MATCHES "drawing offscreen with software GL")
        message(FATAL_ERROR "no fallback line:\n${LAST_OUTPUT}")
    endif()
    if(NOT LAST_OUTPUT MATCHES "exit 0 after 30 ticks")
        message(FATAL_ERROR "the run did not stop on tick 30:\n${LAST_OUTPUT}")
    endif()
    # "P6\n320 180\n255\n" is 15 bytes, then 3 bytes a pixel.
    file(SIZE "${SHOT}" size)
    math(EXPR want "15 + 320 * 180 * 3")
    if(NOT size EQUAL want)
        message(FATAL_ERROR "capture is ${size} bytes, wanted ${want}")
    endif()
    file(READ "${SHOT}" head LIMIT 11)
    if(NOT head MATCHES "^P6\n320 180\n")
        message(FATAL_ERROR "not a 320x180 PPM")
    endif()
    # An explicit driver is not replaced: it fails as it always did, with no fallback line.
    run_env(1 ${NOENV} ${MCE_PLAYER} ${COMMON} -platform.video_driver x11 -capture_tick 5 -capture "${WORK_DIR}/shots/b.ppm")
    if(LAST_OUTPUT MATCHES "drawing offscreen")
        message(FATAL_ERROR "an explicit -platform.video_driver was overridden:\n${LAST_OUTPUT}")
    endif()
    message(STATUS "capture without a display: offscreen fallback, tick 30, 320x180 PPM, explicit driver respected")
else()
    message(FATAL_ERROR "MODE must be save or capture")
endif()
