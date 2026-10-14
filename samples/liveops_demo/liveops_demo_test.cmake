# M-LIVEOPS's demo gates (SBA-25), run with cmake -P: sample_liveops_demo against the local game
# services platform (a folder), a cooked pack standing in for the remote config server and the
# analytics file backend. Achievements and scores reach the platform; a reinstall restores progress
# from the cloud save; a fetched value changes the game at the next launch and airplane mode keeps
# the cached or bundled value; with consent denied no event is written; a recorded run replays to
# the same hash with no provider.

function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "exit ${rc}: ${ARGN}\n${out}\n${err}")
    endif()
    set(LAST_OUTPUT "${out}${err}" PARENT_SCOPE)
    string(REGEX MATCH "hash ([0-9a-f]+)" _ "${out}")
    set(LAST_HASH "${CMAKE_MATCH_1}" PARENT_SCOPE)
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

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(PLATFORM "${WORK_DIR}/platform")
set(GAME "${DEMO}" -ticks 1050)

# Device A, a fresh install: five wins reach the platform and every win's progress is uploaded.
# Recorded for the replay below.
run(${GAME} -save_dir "${WORK_DIR}/device_a" -platform_dir "${PLATFORM}" -record "${WORK_DIR}/run.rec")
expect("tick 1: services SignedIn" "no silent sign-in")
expect("services AchievementSent first_win value 1" "first_win did not reach the platform")
expect("services AchievementSent win_5 value 5" "win_5 did not reach 5 steps on the platform")
expect("services ScoreSent best_time value 3370" "the best time did not reach the platform")
expect("services CloudSaved progress value 5" "the fifth win's progress was not uploaded")
expect("level 6 wins 5 best_time 3370 \\| first_win yes win_5 5/5" "the game's own state")
expect("pending 0" "the outbox did not drain")
file(READ "${PLATFORM}/game_services.jsonl" sent)
if(NOT sent MATCHES "\"achievement\":\"win_5\",\"steps\":5" OR NOT sent MATCHES "\"leaderboard\":\"best_time\"")
    message(FATAL_ERROR "the platform's log lacks win_5 or best_time:\n${sent}")
endif()
file(STRINGS "${WORK_DIR}/device_a/analytics.jsonl" events REGEX "\"name\":\"level_end\"")
list(LENGTH events n)
if(NOT n EQUAL 5)
    message(FATAL_ERROR "expected 5 level_end analytics events, got ${n}")
endif()
set(LIVE_HASH "${LAST_HASH}")

# The replay: same hash, nothing asked of a provider, nothing written.
run(${GAME} -replay "${WORK_DIR}/run.rec")
expect("services CloudSaved progress value 5" "the replay lost an event")
if(NOT LAST_HASH STREQUAL LIVE_HASH)
    message(FATAL_ERROR "the replay hashed ${LAST_HASH}, the live run ${LIVE_HASH}:\n${LAST_OUTPUT}")
endif()

# Device B, a reinstall on the same account: the cloud's progress replaces the empty local save.
run(${GAME} -save_dir "${WORK_DIR}/device_b" -platform_dir "${PLATFORM}" -restore 1 -ticks 300)
expect("services CloudLoaded progress value 5" "the cloud save did not come back")
expect("progress restored: level 6 wins 5" "the restored progress was not applied")
expect("level 7 wins 6" "the game did not continue from the restored progress")
expect("services CloudSaved progress value 6" "the restored device did not upload on top of the cloud")

# Offline platform: wins wait in the outbox and reach the platform on the next online launch.
run(${GAME} -save_dir "${WORK_DIR}/device_c" -platform_dir "${WORK_DIR}/platform_c" -offline 1 -ticks 300)
expect("services SignedOut" "an offline sign-in succeeded")
expect("pending [1-9]" "nothing waited in the outbox")
expect_not("AchievementSent" "an achievement reached an offline platform")
run(${GAME} -save_dir "${WORK_DIR}/device_c" -platform_dir "${WORK_DIR}/platform_c" -ticks 5)
expect("services AchievementSent first_win value 1" "the outbox did not drain online")

# Remote config: the server's value is fetched but the run keeps the bundled one ...
file(WRITE "${WORK_DIR}/server/assets/remote.mcconfig" "{ \"format\": \"mce-config\", \"version\": 1,
  \"guid\": \"5d000000-0000-4000-8000-0000000000c2\", \"values\": { \"level_ticks\": 120, \"motd\": \"Winter event\" } }\n")
run("${MCE_COOK}" "${WORK_DIR}/server" --pack --quiet)
set(RC "${WORK_DIR}/remote")
run(${GAME} -save_dir "${RC}" -server "${WORK_DIR}/server/assets.mce" -ticks 300)
expect("level_ticks 200 motd \"Welcome\"" "the bundled defaults")
expect("fetched 2 values" "the server's values were not fetched")
expect("tick 200: level 1 won" "a fetched value changed the game mid-run")
# ... the next launch plays with it, in airplane mode too (the cache) ...
run(${GAME} -save_dir "${RC}" -offline 1 -ticks 300)
expect("2 values from the last fetch" "the cache was not used")
expect("level_ticks 120 motd \"Winter event\"" "the fetched values were not applied at launch")
expect("tick 120: level 2 won" "the fetched value did not change the game")
# ... a failed fetch keeps them, and a fresh install offline has the bundled values.
run(${GAME} -save_dir "${RC}" -server "${WORK_DIR}/missing.mce" -ticks 5)
expect("fetch failed; keeping the values in force" "a failed fetch")
expect("level_ticks 120" "a failed fetch dropped the cached values")
run(${GAME} -save_dir "${WORK_DIR}/fresh" -offline 1 -ticks 5)
expect("level_ticks 200 motd \"Welcome\"" "offline fresh install without the bundled values")

# Consent denied: the events are dropped and no analytics file is written.
run(${GAME} -save_dir "${WORK_DIR}/denied" -consent denied -ticks 300)
expect("consent denied" "consent")
expect("analytics sent 0 dropped 1" "an event was sent with consent denied")
if(EXISTS "${WORK_DIR}/denied/analytics.jsonl")
    message(FATAL_ERROR "consent was denied but analytics.jsonl was written")
endif()

message(STATUS "liveops demo holds: services, replay, cloud restore, offline outbox, remote config, consent")
