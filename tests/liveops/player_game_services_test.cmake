# Game services end to end (M-LIVEOPS step 2), run with cmake -P: mce_player -headless with
# -liveops 1 -liveops.provider local and a cooked .mcservices. Achievements and a score earned
# offline wait in the saved outbox and are sent on a later launch; a cloud save moves to a second
# device (a second save folder sharing the platform folder), and a conflict is settled by the game.

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

set(SCENE_GUID "6f000000-0000-4000-8000-000000000003")
file(REMOVE_RECURSE "${WORK_DIR}")
file(WRITE "${WORK_DIR}/game/assets/scenes/main.mcscene" "{ \"format\": \"mce-scene\", \"version\": 1, \"guid\": \"${SCENE_GUID}\", \"name\": \"main\",
  \"entities\": [ { \"name\": \"E\", \"components\": { \"LocalTransform\": { \"position\": [0, 0, 0] } } } ] }\n")
file(WRITE "${WORK_DIR}/game/assets/game.mcservices" "{ \"format\": \"mce-services\", \"version\": 1,
  \"guid\": \"6f000000-0000-4000-8000-0000000000d1\",
  \"achievements\": [ { \"id\": \"first_win\", \"ios\": \"com.studio.first_win\", \"android\": \"CgkIfirst\" },
                      { \"id\": \"win_10\", \"steps\": 10 } ],
  \"leaderboards\": [ { \"id\": \"best_time\", \"order\": \"lower_is_better\" } ],
  \"cloud_saves\": [ \"progress\" ] }\n")
run(0 "${MCE_COOK}" "${WORK_DIR}/game" --pack --quiet)

set(PLATFORM "${WORK_DIR}/platform")
set(BASE "${MCE_PLAYER}" -pack "${WORK_DIR}/game/assets.mce" -scene ${SCENE_GUID} -headless 1 -platform.error_box 0
    -ticks 10 -liveops 1 -liveops.provider local -game_services.dir "${PLATFORM}")
set(A ${BASE} -save_dir "${WORK_DIR}/device_a")
set(B ${BASE} -save_dir "${WORK_DIR}/device_b")

# Offline: everything counts on the device and waits.
run(0 ${A} -game_services.offline 1 -game_services_test "2:unlock:first_win 2:progress:win_10:3 3:score:best_time:4200")
expect("game_services: provider 'local', 2 achievements, 1 leaderboards, 1 cloud saves, 0 waiting" "offline start")
expect("SignedOut - value 0" "offline sign-in")
expect_not("AchievementSent|ScoreSent" "sent while offline")
if(EXISTS "${PLATFORM}/game_services.jsonl")
    message(FATAL_ERROR "the platform heard from an offline device")
endif()

# Online on the next launch: the outbox drains, once.
run(0 ${A})
expect("3 waiting" "the outbox was not saved")
expect("SignedIn - value 0" "sign-in")
expect("AchievementSent first_win value 1" "first_win sent")
expect("AchievementSent win_10 value 3" "win_10 progress sent")
expect("ScoreSent best_time value 4200" "score sent")
file(READ "${PLATFORM}/game_services.jsonl" log)
if(NOT log MATCHES "\"achievement\":\"first_win\",\"steps\":1" OR NOT log MATCHES "\"leaderboard\":\"best_time\",\"score\":4200")
    message(FATAL_ERROR "the platform log lacks what was sent:\n${log}")
endif()
run(0 ${A})
expect("0 waiting" "sent twice")

# A cloud save moves to device B.
run(0 ${A} -game_services_test "2:write:progress:level3 2:cloud_save:progress")
expect("CloudSaved progress value 1" "device A upload")
run(0 ${B} -game_services_test "2:cloud_load:progress")
expect("CloudLoaded progress value 1" "device B download")
expect("progress is now \"level3\"" "device B's save")

# Both play on; A's upload conflicts and A keeps the cloud's.
run(0 ${B} -game_services_test "2:write:progress:level5b 2:cloud_save:progress")
expect("CloudSaved progress value 2" "device B upload")
run(0 ${A} -game_services_test "2:write:progress:level4a 2:cloud_save:progress 7:keep_cloud:progress")
expect("CloudConflict progress value 2" "the conflict")
expect("progress is now \"level5b\"" "keep_cloud")

# Mistakes are errors.
run(1 "${MCE_PLAYER}" -pack "${WORK_DIR}/game/assets.mce" -scene ${SCENE_GUID} -headless 1 -platform.error_box 0
    -ticks 1 -game_services_test "2:unlock:first_win")
run(1 ${A} -game_services_test "2:unlock")
run(1 ${A} -game_services_test "2:jump:first_win")

message(STATUS "game services hold: offline outbox, sent once, cloud save across devices, conflict settled")
