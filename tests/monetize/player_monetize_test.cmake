# Purchases and ads end to end (M-MONETIZE step 2), run with cmake -P: a project with a store
# catalog cooks to assets.mce, then mce_player -headless plays it with the fake store and ad network
# (-monetize 1 -monetize.provider fake) and a -monetize_test script, once per outcome the fakes
# cover. The entitlement bought in the first run is still owned in the second (same -save_dir).

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

set(SCENE_GUID "6e000000-0000-4000-8000-000000000001")
file(REMOVE_RECURSE "${WORK_DIR}")
file(WRITE "${WORK_DIR}/game/assets/scenes/main.mcscene" "{ \"format\": \"mce-scene\", \"version\": 1, \"guid\": \"${SCENE_GUID}\", \"name\": \"main\",
  \"entities\": [ { \"name\": \"E\", \"components\": { \"LocalTransform\": { \"position\": [0, 0, 0] } } } ] }\n")
file(WRITE "${WORK_DIR}/game/assets/store.mcstore" "{ \"format\": \"mce-store\", \"version\": 1, \"guid\": \"6e000000-0000-4000-8000-000000000002\",
  \"products\": [ { \"id\": \"remove_ads\", \"type\": \"non_consumable\" }, { \"id\": \"gems_100\", \"type\": \"consumable\" } ],
  \"ads\": [ { \"id\": \"continue\", \"format\": \"rewarded\" }, { \"id\": \"level_end\", \"format\": \"interstitial\" },
             { \"id\": \"menu\", \"format\": \"banner\" } ] }\n")
run(0 "${MCE_COOK}" "${WORK_DIR}/game" --pack --quiet)

set(PLAY "${MCE_PLAYER}" -pack "${WORK_DIR}/game/assets.mce" -scene ${SCENE_GUID} -headless 1 -platform.error_box 0
    -save_dir "${WORK_DIR}/saves" -monetize 1 -monetize.provider fake)

# Success: prices, a consumable and a non-consumable, a rewarded ad watched to the end, an interstitial,
# a banner. A fullscreen ad pauses the sim, so 120 app ticks reach fewer sim ticks.
run(0 ${PLAY} -ticks 120 -monetize.fake.ad_updates 10 -monetize.fake.reward 2
    -monetize_test "1:query 2:buy:gems_100 3:buy:remove_ads 5:show:continue 7:show:level_end 9:banner:menu")
expect("store ProductInfo gems_100 request 1 price \\$0.99" "no price")
expect("store ProductsReady - request 1" "the product query did not finish")
expect("store Purchased gems_100 request 2" "the consumable was not bought")
expect("store Purchased remove_ads request 3" "the non-consumable was not bought")
expect("ad Shown continue request 4" "the rewarded ad was not shown")
expect("ad Rewarded continue request 4 reward 2" "the rewarded ad gave no reward")
expect("ad Closed continue request 4" "the rewarded ad did not close")
expect("ad Closed level_end request 5" "the interstitial did not close")
expect_not("ad Rewarded level_end" "an interstitial rewarded")
expect("ad Shown menu request 6" "the banner was not shown")
expect("a fullscreen ad is up" "the fullscreen ad did not pause the sim")
expect(": 100 ticks" "the sim ran while the ads were up (120 app ticks, 2 x 10 of them paused)")

# Relaunch: the entitlement is still owned, offline; buying it again is refused before the store.
run(0 ${PLAY} -ticks 10 -monetize.provider none -monetize_test "2:buy:remove_ads")
expect("monetize: owns remove_ads" "the entitlement did not survive the relaunch")
expect("buy remove_ads -> request 0" "an owned non-consumable was sold twice")

# Cancel, failure, pending, no fill, closed early.
file(REMOVE_RECURSE "${WORK_DIR}/saves")
run(0 ${PLAY} -ticks 20 -monetize.fake.purchase cancel -monetize_test "1:buy:remove_ads")
expect("store Cancelled remove_ads request 1" "cancel")
run(0 ${PLAY} -ticks 20 -monetize.fake.purchase failure -monetize_test "1:buy:remove_ads")
expect("store Failed remove_ads request 1" "failure")
run(0 ${PLAY} -ticks 40 -monetize.fake.purchase pending -monetize.fake.pending_updates 10 -monetize_test "1:buy:remove_ads")
expect("store Pending remove_ads request 1" "pending")
expect("store Purchased remove_ads request 1" "the pending purchase never completed")
run(0 ${PLAY} -ticks 20 -monetize.fake.fill nofill -monetize_test "1:show:level_end 2:banner:menu")
expect("ad NoFill level_end request 1" "no fill (interstitial)")
expect("ad NoFill menu request 2" "no fill (banner)")
expect_not("a fullscreen ad is up" "a no-fill paused the sim")
run(0 ${PLAY} -ticks 40 -monetize.fake.close close -monetize.fake.ad_updates 5 -monetize_test "1:show:continue")
expect("ad Closed continue request 1" "the early close")
expect_not("ad Rewarded" "an ad closed early still rewarded")

# Restore after a reinstall: the fake account owns remove_ads.
file(REMOVE_RECURSE "${WORK_DIR}/saves")
run(0 ${PLAY} -ticks 10 -monetize.fake.owned remove_ads -monetize_test "1:restore")
expect("store Restored remove_ads request 1" "restore")
expect("store RestoreFinished - request 1" "restore did not finish")

# Off unless turned on: no -monetize, -monetize_test is an error.
run(1 "${MCE_PLAYER}" -pack "${WORK_DIR}/game/assets.mce" -scene ${SCENE_GUID} -headless 1 -platform.error_box 0 -ticks 5
    -monetize_test "1:query")

message(STATUS "purchases and ads hold: success, relaunch, cancel, failure, pending, no fill, early close, restore")
