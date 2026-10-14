# Remote config end to end (M-LIVEOPS step 3), run with cmake -P: a game whose defaults come from a
# cooked .mcconfig, and a second cooked pack standing in for the server (-remote_config.file). The
# fetched values are saved but not applied mid-run; the next launch starts with them. A fetched key
# the defaults lack, or of another type, is ignored.

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

function(expect_not pattern what)
    if(LAST_OUTPUT MATCHES "${pattern}")
        message(FATAL_ERROR "${what} (unexpected '${pattern}'):\n${LAST_OUTPUT}")
    endif()
endfunction()

set(SCENE_GUID "6f000000-0000-4000-8000-000000000002")
file(REMOVE_RECURSE "${WORK_DIR}")
file(WRITE "${WORK_DIR}/game/assets/scenes/main.mcscene" "{ \"format\": \"mce-scene\", \"version\": 1, \"guid\": \"${SCENE_GUID}\", \"name\": \"main\",
  \"entities\": [ { \"name\": \"E\", \"components\": { \"LocalTransform\": { \"position\": [0, 0, 0] } } } ] }\n")
file(WRITE "${WORK_DIR}/game/assets/remote.mcconfig" "{ \"format\": \"mce-config\", \"version\": 1,
  \"guid\": \"6f000000-0000-4000-8000-0000000000c1\",
  \"values\": { \"ad_every_levels\": 3, \"gem_bonus\": 1.5, \"winter_event\": false, \"motd\": \"Welcome\" } }\n")
run(0 "${MCE_COOK}" "${WORK_DIR}/game" --pack --quiet)

# The \"server\": newer values, one key the game does not know and one of the wrong type.
file(WRITE "${WORK_DIR}/server/assets/remote.mcconfig" "{ \"format\": \"mce-config\", \"version\": 1,
  \"guid\": \"6f000000-0000-4000-8000-0000000000c2\",
  \"values\": { \"ad_every_levels\": 5, \"winter_event\": true, \"motd\": \"Snow!\", \"unknown_key\": 1,
                \"gem_bonus\": \"lots\" } }\n")
run(0 "${MCE_COOK}" "${WORK_DIR}/server" --pack --quiet)

set(SAVES "${WORK_DIR}/saves")
set(PLAY "${MCE_PLAYER}" -pack "${WORK_DIR}/game/assets.mce" -scene ${SCENE_GUID} -headless 1 -platform.error_box 0
    -save_dir "${SAVES}" -ticks 5 -liveops 1 -remote_config.dump 1)

# No server: the cooked defaults.
run(0 ${PLAY})
expect("remote_config: 4 values from defaults, fetcher 'none'" "defaults run")
expect("remote_config v0: ad_every_levels = 3" "default int")
expect("remote_config v0: motd = \"Welcome\"" "default string")

# The first fetch: saved, not applied mid-run.
run(0 ${PLAY} -remote_config.file "${WORK_DIR}/server/assets.mce")
expect("remote_config: 4 values from defaults, fetcher 'file'" "fetch run start")
expect("fetched 'unknown_key' \\(int\\) is not a default of that type; ignored" "unknown key")
expect("fetched 'gem_bonus' \\(string\\) is not a default of that type; ignored" "wrong type")
expect("remote_config: fetched 4 values \\(waiting for the next launch or apply_fetched\\)" "fetch result")
expect_not("remote_config v1" "fetched values applied mid-run")
expect_not("ad_every_levels = 5" "fetched values applied mid-run")

# The next launch, even with no server, starts from the fetched values.
run(0 ${PLAY})
expect("remote_config: 4 values from the last fetch, fetcher 'none'" "cached run")
expect("remote_config v0: ad_every_levels = 5" "fetched int")
expect("remote_config v0: gem_bonus = 1.5" "the ignored value keeps its default")
expect("remote_config v0: winter_event = true" "fetched bool")
expect("remote_config v0: motd = \"Snow!\"" "fetched string")

# -liveops.provider none turns the fetcher off too; an unreadable pack keeps the values in force.
run(0 ${PLAY} -liveops.provider none -remote_config.file "${WORK_DIR}/server/assets.mce")
expect("fetcher 'none'" "provider none")
run(0 ${PLAY} -remote_config.file "${WORK_DIR}/missing.mce")
expect("remote_config: fetch failed; keeping the values in force" "missing pack")
expect("remote_config v0: ad_every_levels = 5" "values after a failed fetch")

message(STATUS "remote config holds: defaults, fetch saved for the next launch, schema enforced, failure kept")
