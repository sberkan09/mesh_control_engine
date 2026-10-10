# Data mods end to end (ADR-0034), run with cmake -P:
#   a game project cooks to assets.mce; a mod project (mod.json, base = that pack) overrides the
#   game's scene with a bigger one and cooks to its own pack; mce_player -headless then plays the
#   game with the mod in a mods folder (more entities), with -mods 0 (the game's own), and with a
#   load_order.txt that leaves the mod out.

function(run expected_rc)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL expected_rc)
        message(FATAL_ERROR "expected exit ${expected_rc}, got ${rc}: ${ARGN}\n${out}\n${err}")
    endif()
    set(LAST_OUTPUT "${out}${err}" PARENT_SCOPE)
endfunction()

function(scene path guid count)
    set(entities "")
    foreach(i RANGE 1 ${count})
        if(i GREATER 1)
            string(APPEND entities ",\n")
        endif()
        string(APPEND entities "    { \"name\": \"E${i}\", \"components\": { \"LocalTransform\": { \"position\": [${i}, 0, 0] } } }")
    endforeach()
    file(WRITE "${path}" "{\n  \"format\": \"mce-scene\",\n  \"version\": 1,\n  \"guid\": \"${guid}\",\n  \"name\": \"main\",\n  \"entities\": [\n${entities}\n  ]\n}\n")
endfunction()

set(SCENE_GUID "6d000000-0000-4000-8000-000000000001")
file(REMOVE_RECURSE "${WORK_DIR}")

# The game: three entities.
scene("${WORK_DIR}/game/assets/scenes/main.mcscene" ${SCENE_GUID} 3)
run(0 "${MCE_COOK}" "${WORK_DIR}/game" --pack --quiet)

# The mod: the same scene GUID with five entities.
file(WRITE "${WORK_DIR}/bigger/mod.json"
     "{ \"format\": \"mce-mod\", \"version\": 1, \"id\": \"bigger\", \"name\": \"Bigger Level\", \"base\": \"../game/assets.mce\" }\n")
scene("${WORK_DIR}/bigger/assets/scenes/main_big.mcscene" ${SCENE_GUID} 5)
run(0 "${MCE_COOK}" "${WORK_DIR}/bigger" --pack --quiet)
if(NOT LAST_OUTPUT MATCHES "mod on 1 base asset\\(s\\), 1 override\\(s\\)")
    message(FATAL_ERROR "mod cook summary missing:\n${LAST_OUTPUT}")
endif()
file(MAKE_DIRECTORY "${WORK_DIR}/mods")
file(COPY_FILE "${WORK_DIR}/bigger/assets.mce" "${WORK_DIR}/mods/bigger.mce")
file(WRITE "${WORK_DIR}/mods/readme.mce" "not a pack")  # skipped with a warning, never fatal

set(PLAY "${MCE_PLAYER}" -pack "${WORK_DIR}/game/assets.mce" -scene ${SCENE_GUID} -headless 1 -ticks 10 -platform.error_box 0)

run(0 ${PLAY} -mods_dir "${WORK_DIR}/mods")
if(NOT LAST_OUTPUT MATCHES "1 mod\\(s\\) loaded" OR NOT LAST_OUTPUT MATCHES "10 ticks, 5 entities")
    message(FATAL_ERROR "the mod did not replace the scene:\n${LAST_OUTPUT}")
endif()
if(NOT LAST_OUTPUT MATCHES "mod skipped: [^\n]*readme.mce")
    message(FATAL_ERROR "the broken pack was not reported:\n${LAST_OUTPUT}")
endif()

run(0 ${PLAY} -mods_dir "${WORK_DIR}/mods" -mods 0)
if(NOT LAST_OUTPUT MATCHES "10 ticks, 3 entities")
    message(FATAL_ERROR "-mods 0 still loaded the mod:\n${LAST_OUTPUT}")
endif()

file(WRITE "${WORK_DIR}/mods/load_order.txt" "# nothing enabled\n")
run(0 ${PLAY} -mods_dir "${WORK_DIR}/mods")
if(NOT LAST_OUTPUT MATCHES "10 ticks, 3 entities" OR NOT LAST_OUTPUT MATCHES "not listed in load_order.txt")
    message(FATAL_ERROR "load_order.txt did not leave the mod out:\n${LAST_OUTPUT}")
endif()

message(STATUS "data mods hold: override, -mods 0, load_order.txt")
