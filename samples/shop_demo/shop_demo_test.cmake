# M-MONETIZE's demo gates (SBA-21), run with cmake -P: sample_shop_demo against the fake store and
# ad network. Purchase, cancel, restore and relaunch keep the entitlement; with remove_ads owned no
# ad is requested; a recorded run replays to the same hash with no provider.

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
set(SAVES "${WORK_DIR}/saves")
set(GAME "${SHOP}" -ticks 1850 -monetize.fake.ad_updates 20)

# A fresh install: gems bought and granted, a rewarded continue, two interstitials, then remove_ads
# bought and the rest of the interstitials skipped. Recorded for the replay below.
run(${GAME} -save_dir "${SAVES}" -record "${WORK_DIR}/run.rec")
expect("store ProductInfo remove_ads request 1" "no price")
expect("store Purchased gems_100 request 2" "the consumable was not bought")
expect("gems 100" "the consumable was not granted")
expect("unfinished 0" "the consumable was granted but not finished")
expect("ad Rewarded continue" "the rewarded continue gave no reward")
expect("continues 1 restarts 0" "the rewarded ad did not revive the runner")
expect("store Purchased remove_ads" "the non-consumable was not bought")
expect("interstitials 2 skipped 4" "interstitials before and after remove_ads")
expect("\\([1-9][0-9]* paused by ads" "the fullscreen ads did not pause the sim")
expect("owns remove_ads\n" "remove_ads is not owned")
set(LIVE_HASH "${LAST_HASH}")

# The replay: same hash, nothing asked of a provider, nothing persisted.
run(${GAME} -replay "${WORK_DIR}/run.rec")
expect("store Purchased remove_ads" "the replay lost the purchase")
expect("\\(0 paused by ads" "a replay has no ad on screen")
if(NOT LAST_HASH STREQUAL LIVE_HASH)
    message(FATAL_ERROR "the replay hashed ${LAST_HASH}, the live run ${LIVE_HASH}:\n${LAST_OUTPUT}")
endif()

# A relaunch, offline: remove_ads is still owned, no ad network is started and no ad is requested.
run(${GAME} -save_dir "${SAVES}" -provider none)
expect("owns remove_ads at launch" "the entitlement did not survive the relaunch")
expect("ads removed at launch" "the ad network was started with ads removed")
expect("continues 1 restarts 0 \\| interstitials 0 skipped 6" "an ad was requested with ads removed")
expect_not("show (continue|level_end)" "an ad was asked for with ads removed")

# Cancel: nothing is owned, nothing granted, every interstitial still shows.
run(${GAME} -save_dir "${WORK_DIR}/cancel" -monetize.fake.purchase cancel)
expect("store Cancelled gems_100" "cancel (consumable)")
expect("store Cancelled remove_ads" "cancel (non-consumable)")
expect("gems 0" "a cancelled purchase was granted")
expect("interstitials 6 skipped 0" "a cancelled remove_ads removed ads")
expect_not("owns remove_ads" "a cancelled purchase is owned")

# A reinstall: the store account owns remove_ads, restore brings it back from the first tick on.
run(${GAME} -save_dir "${WORK_DIR}/reinstall" -monetize.fake.owned remove_ads -restore 1 -shop 0)
expect("store Restored remove_ads request 2" "restore")
expect("store RestoreFinished - request 2" "restore did not finish")
expect("interstitials 0 skipped 6" "ads after the restore")
expect("owns remove_ads\n" "the restored entitlement is not owned")
run(${GAME} -save_dir "${WORK_DIR}/reinstall" -provider none -shop 0)
expect("owns remove_ads at launch" "the restored entitlement was not persisted")

message(STATUS "shop demo holds: purchase, replay, relaunch, cancel, restore")
