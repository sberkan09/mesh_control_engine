# Live services end to end (M-LIVEOPS step 1), run with cmake -P: mce_player -headless with
# -liveops 1 -liveops.provider local. A crash report an earlier run left in the crash folder is
# copied to <save folder>/crash_outbox/ only once consent is granted, renamed .crash.sent, and never
# sent twice. With REAL_CRASH the report comes from a real -crash_test run.

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

set(SCENE_GUID "6f000000-0000-4000-8000-000000000001")
file(REMOVE_RECURSE "${WORK_DIR}")
file(WRITE "${WORK_DIR}/game/assets/scenes/main.mcscene" "{ \"format\": \"mce-scene\", \"version\": 1, \"guid\": \"${SCENE_GUID}\", \"name\": \"main\",
  \"entities\": [ { \"name\": \"E\", \"components\": { \"LocalTransform\": { \"position\": [0, 0, 0] } } } ] }\n")
run(0 "${MCE_COOK}" "${WORK_DIR}/game" --pack --quiet)

set(CRASHES "${WORK_DIR}/crashes")
set(SAVES "${WORK_DIR}/saves")
set(PLAY "${MCE_PLAYER}" -pack "${WORK_DIR}/game/assets.mce" -scene ${SCENE_GUID} -headless 1 -platform.error_box 0
    -crash_dir "${CRASHES}" -save_dir "${SAVES}" -ticks 5)

# The report an earlier run left.
if(REAL_CRASH)
    execute_process(COMMAND ${PLAY} -crash_test 1 RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
    if(rc EQUAL 0)
        message(FATAL_ERROR "-crash_test 1 exited cleanly")
    endif()
else()
    file(WRITE "${CRASHES}/mce_player-20261014T050000Z-1.crash" "mce crash report (test)\n")
endif()
file(GLOB reports "${CRASHES}/*.crash")
list(LENGTH reports count)
if(NOT count EQUAL 1)
    message(FATAL_ERROR "expected one crash report in ${CRASHES}, found ${count}")
endif()
get_filename_component(report "${reports}" NAME)

# Denied, and auto with no consent form on this platform: the report stays on this device.
run(0 ${PLAY} -liveops 1 -liveops.provider local -liveops.consent denied)
expect("liveops: analytics 'file', crash reports 'outbox' \\(1 waiting\\), consent denied" "denied run")
run(0 ${PLAY} -liveops 1 -liveops.provider local)
expect("consent unknown" "auto run")
if(EXISTS "${SAVES}/crash_outbox/${report}" OR NOT EXISTS "${CRASHES}/${report}")
    message(FATAL_ERROR "a report left the device without consent")
endif()

# Granted: sent once, kept as .sent.
run(0 ${PLAY} -liveops 1 -liveops.provider local -liveops.consent granted)
expect("liveops: crash report ${report} sent" "the report was not sent")
if(NOT EXISTS "${SAVES}/crash_outbox/${report}" OR NOT EXISTS "${CRASHES}/${report}.sent")
    message(FATAL_ERROR "the report did not reach the outbox or was not marked sent")
endif()
file(READ "${SAVES}/crash_outbox/${report}" sent_text)
if(NOT sent_text MATCHES "mce")
    message(FATAL_ERROR "the outbox copy is not the report:\n${sent_text}")
endif()
run(0 ${PLAY} -liveops 1 -liveops.provider local -liveops.consent granted)
expect("\\(0 waiting\\)" "a sent report was found again")

# Off unless turned on; bad values are errors.
file(REMOVE_RECURSE "${SAVES}/crash_outbox")
run(0 ${PLAY} -liveops.consent granted)
if(EXISTS "${SAVES}/crash_outbox")
    message(FATAL_ERROR "live services ran without -liveops 1")
endif()
run(1 ${PLAY} -liveops 1 -liveops.consent maybe)
run(1 ${PLAY} -liveops 1 -liveops.provider vendor)

message(STATUS "live services hold: denied, unknown, granted, sent once, off by default")
