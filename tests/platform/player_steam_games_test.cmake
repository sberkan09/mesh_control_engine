# Steam game services end to end (M-STEAM step 2), run with cmake -P: the stock player with
# -steam.app_id and the stand-in steam_api, a cooked .mcservices, and -game_services_test. With no
# provider named, a running Steam is the game services backend; an achievement goes out under its
# "steam" name, a leaderboard under its catalog id, and a cloud save lands in Steam Cloud.

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

set(SCENE_GUID "6f000000-0000-4000-8000-000000000013")
file(REMOVE_RECURSE "${WORK_DIR}")
file(WRITE "${WORK_DIR}/game/assets/scenes/main.mcscene" "{ \"format\": \"mce-scene\", \"version\": 1, \"guid\": \"${SCENE_GUID}\", \"name\": \"main\",
  \"entities\": [ { \"name\": \"E\", \"components\": { \"LocalTransform\": { \"position\": [0, 0, 0] } } } ] }\n")
file(WRITE "${WORK_DIR}/game/assets/game.mcservices" "{ \"format\": \"mce-services\", \"version\": 1,
  \"guid\": \"6f000000-0000-4000-8000-0000000000d3\",
  \"achievements\": [ { \"id\": \"first_win\", \"steam\": \"ACH_FIRST_WIN\" }, { \"id\": \"win_10\", \"steps\": 10 } ],
  \"leaderboards\": [ { \"id\": \"best_time\", \"order\": \"lower_is_better\" } ],
  \"cloud_saves\": [ \"progress\" ] }\n")
run(0 "${MCE_COOK}" "${WORK_DIR}/game" --pack --quiet)

set(ENV{MCE_STEAM_STUB_BOARDS} "best_time")
run(0 "${MCE_PLAYER}" -pack "${WORK_DIR}/game/assets.mce" -scene ${SCENE_GUID} -headless 1 -platform.error_box 0
    -ticks 10 -liveops 1 -save_dir "${WORK_DIR}/save" -steam.app_id 480 -steam.library "${STEAM_STUB}"
    -game_services_test "2:unlock:first_win 2:progress:win_10:3 3:score:best_time:4200 3:write:progress:level3 3:cloud_save:progress")
expect("Steam on \\(app 480, Stub Player\\)" "Steam did not start")
expect("game services on steam" "Steam is not the game services backend")
expect("SignedIn - value 0" "sign-in")
expect("AchievementSent first_win value 1" "first_win (as ACH_FIRST_WIN)")
expect("AchievementSent win_10 value 3" "win_10 progress")
expect("ScoreSent best_time value 4200" "the score")
expect("CloudSaved progress value 1" "the cloud save")

message(STATUS "Steam game services hold: sign-in, achievements by steam name, a leaderboard by id, Steam Cloud")
