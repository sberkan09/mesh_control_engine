# Steam Input end to end (M-STEAM step 4), run with cmake -P: the stock player on the stand-in
# steam_api with one controller holding Jump (MCE_STEAM_STUB_PADS, MCE_STEAM_STUB_INPUT). Steam Input
# starts with the input map's actions, reads the controller and the held action every frame, and a
# headless run's state hash stays what it is without Steam (headless plays scripted input only).
# -steam.write_input_config writes the action manifest and the four default configurations.

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

set(SCENE_GUID "6f000000-0000-4000-8000-000000000024")
file(REMOVE_RECURSE "${WORK_DIR}")
file(WRITE "${WORK_DIR}/game/assets/scenes/main.mcscene" "{ \"format\": \"mce-scene\", \"version\": 1, \"guid\": \"${SCENE_GUID}\", \"name\": \"main\",
  \"entities\": [ { \"name\": \"Level\", \"components\": { \"LocalTransform\": { \"position\": [0, 0, 0] } } } ] }\n")
run(0 "${MCE_COOK}" "${WORK_DIR}/game" --pack --quiet)
set(PLAY "${MCE_PLAYER}" -pack "${WORK_DIR}/game/assets.mce" -scene ${SCENE_GUID} -headless 1 -platform.error_box 0 -ticks 10
    -save_dir "${WORK_DIR}/save")

run(0 ${PLAY})
string(REGEX MATCH "state_hash [0-9a-f]+" plain "${LAST_OUTPUT}")

set(ENV{MCE_STEAM_STUB_PADS} "1")
set(ENV{MCE_STEAM_STUB_INPUT} "Jump,Move=0.5:1")
run(0 ${PLAY} -steam.app_id 480 -steam.library "${STEAM_STUB}" -steam.write_input_config "${WORK_DIR}/steam_input")
expect("Steam Input on \\([0-9]+ actions in the set \"Game\"\\)" "Steam Input did not start")
expect("Steam Input reads 1 controller" "the controller")
expect("steam stub: input Jump held" "the digital action")
expect("steam stub: input Move held" "the analog action")
expect("${plain}" "Steam Input changed a headless run's state hash")

foreach(file game_actions_480.vdf controller_neptune.vdf controller_xboxone.vdf controller_ps4.vdf controller_switch_pro.vdf)
    if(NOT EXISTS "${WORK_DIR}/steam_input/${file}")
        message(FATAL_ERROR "-steam.write_input_config did not write ${file}")
    endif()
endforeach()
file(READ "${WORK_DIR}/steam_input/game_actions_480.vdf" manifest)
if(NOT manifest MATCHES "\"Jump\"\t\t\"#Action_Jump\"" OR NOT manifest MATCHES "\"controller_neptune.vdf\"")
    message(FATAL_ERROR "the action manifest lacks Jump or the Deck's configuration:\n${manifest}")
endif()

# Off with -steam.input 0: no Steam Input, Steam itself still on.
run(0 ${PLAY} -steam.app_id 480 -steam.library "${STEAM_STUB}" -steam.input 0)
if(LAST_OUTPUT MATCHES "Steam Input on")
    message(FATAL_ERROR "-steam.input 0 still started Steam Input:\n${LAST_OUTPUT}")
endif()
# The config needs the app id that names the manifest.
run(1 ${PLAY} -steam.write_input_config "${WORK_DIR}/no_app")
expect("needs -steam.app_id" "the missing app id")

message(STATUS "Steam Input reads the controller's actions; its configuration matches the input map")
