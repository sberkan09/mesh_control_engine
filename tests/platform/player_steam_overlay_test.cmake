# The Steam overlay and rich presence end to end (M-STEAM step 3), run with cmake -P: the stock
# player on the stand-in steam_api with a scene carrying a RichPresence. The stand-in opens its
# overlay on one frame and closes it on a later one (MCE_STEAM_STUB_OVERLAY): the game holds like a
# lost focus, and the presence reaches Steam once, key by key.

function(run expected_rc)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL expected_rc)
        message(FATAL_ERROR "expected exit ${expected_rc}, got ${rc}: ${ARGN}\n${out}\n${err}")
    endif()
    set(LAST_OUTPUT "${out}${err}" PARENT_SCOPE)
endfunction()

function(expect pattern what)
    if(NOT LAST_OUTPUT MATCHES "${pattern}")
        message(FATAL_ERROR "${what} (no match for '${pattern}'):\n${LAST_OUTPUT}")
    endif()
endfunction()

set(SCENE_GUID "6f000000-0000-4000-8000-000000000023")
file(REMOVE_RECURSE "${WORK_DIR}")
file(WRITE "${WORK_DIR}/game/assets/scenes/main.mcscene" "{ \"format\": \"mce-scene\", \"version\": 1, \"guid\": \"${SCENE_GUID}\", \"name\": \"main\",
  \"entities\": [ { \"name\": \"Level\", \"components\": { \"LocalTransform\": { \"position\": [0, 0, 0] },
    \"RichPresence\": { \"status\": \"Exploring the Ashen Court\", \"steam_display\": \"#StatusInLevel\",
                        \"key1\": \"level\", \"value1\": \"Ashen Court\" } } } ] }\n")
run(0 "${MCE_COOK}" "${WORK_DIR}/game" --pack --quiet)

set(ENV{MCE_STEAM_STUB_OVERLAY} "3,6")
run(0 "${MCE_PLAYER}" -pack "${WORK_DIR}/game/assets.mce" -scene ${SCENE_GUID} -headless 1 -platform.error_box 0
    -ticks 10 -save_dir "${WORK_DIR}/save" -steam.app_id 480 -steam.library "${STEAM_STUB}")
expect("Steam on \\(app 480, Stub Player\\)" "Steam did not start")
expect("rich presence status=Exploring the Ashen Court" "the status key")
expect("rich presence steam_display=#StatusInLevel" "the display token")
expect("rich presence level=Ashen Court" "the substitution")
expect("Steam overlay open: the game holds.*Steam overlay closed" "the overlay")
string(REGEX MATCHALL "rich presence status=" sent "${LAST_OUTPUT}")
list(LENGTH sent n)
if(NOT n EQUAL 1)
    message(FATAL_ERROR "the presence was sent ${n} times, not once:\n${LAST_OUTPUT}")
endif()

message(STATUS "Steam overlay holds the game; rich presence sent once")
