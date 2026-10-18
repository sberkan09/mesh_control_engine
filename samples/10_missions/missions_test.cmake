# 10_missions' headless gates (M-MISSION step 4), run with cmake -P: mce_player plays the cooked pack with the
# scripted run in input.txt and the script's log lines pin when each mission starts and completes (script ticks
# count from 0; mission.start in tick N makes the mission active in tick N + 1). Gates:
#   1. the three missions complete at the pinned ticks, in order, and a second play ends on the same state hash;
#   2. a delivery that never arrives fails mission 3 on its 2.5 s time limit (tick 83 + 150 = 233);
#   3. a save written in the middle of mission 3 resumes there: mission 1 and 2 are not started again, the delivery
#      completes at the same tick (176) as in the uninterrupted run, and two resumes end on the same hash.
# Input ticks are 1-based and count from the start of the run (a resumed run starts its script over at 1).

function(play out_var)
    execute_process(COMMAND "${MCE_PLAYER}" -headless 1 -pack "${PACK}" ${ARGN}
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "mce_player exited ${rc}\n${out}\n${err}")
    endif()
    set(${out_var} "${out}${err}" PARENT_SCOPE)
endfunction()

function(expect text pattern what)
    if(NOT text MATCHES "${pattern}")
        message(FATAL_ERROR "${what} (no match for '${pattern}'):\n${text}")
    endif()
endfunction()

function(expect_not text pattern what)
    if(text MATCHES "${pattern}")
        message(FATAL_ERROR "${what} (unexpected '${pattern}'):\n${text}")
    endif()
endfunction()

function(hash_of text out_var)
    if(NOT text MATCHES "state_hash ([0-9a-f]+)")
        message(FATAL_ERROR "no state hash:\n${text}")
    endif()
    set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# 1. The whole run: reach (tick 58), kill (82), deliver (176).
play(first -ticks 300 -input "${INPUT}")
play(second -ticks 300 -input "${INPUT}")
expect("${first}" "300 ticks, 16 entities, state_hash" "the run did not end after 300 ticks with 16 entities")
expect_not("${first}" "\\[error\\]|\\[warn\\]|does not load|failed at" "the run logged a problem")
expect("${first}" "script: mission 1 active at tick 11\n" "mission 1 did not start after the briefing")
expect("${first}" "script: mission 1 complete at tick 58\n" "mission 1 (reach the zone)")
expect("${first}" "script: mission 2 active at tick 59\n" "mission 2 was not chained after mission 1")
expect("${first}" "script: mission 2 complete at tick 82\n" "mission 2 (kill the target)")
expect("${first}" "script: mission 3 active at tick 83\n" "mission 3 was not chained after mission 2")
expect("${first}" "script: mission 3 complete at tick 176\n" "mission 3 (deliver within the time limit)")
hash_of("${first}" hash_a)
hash_of("${second}" hash_b)
if(NOT hash_a STREQUAL hash_b)
    message(FATAL_ERROR "two plays differ: ${hash_a} vs ${hash_b}")
endif()

# 2. The player stops after the kill: the delivery runs out of time.
file(WRITE "${WORK_DIR}/late.input" "15-60 Move=0,1\n70-100 Fire\n")
play(late -ticks 300 -input "${WORK_DIR}/late.input")
expect("${late}" "script: mission 2 complete at tick 82\n" "mission 2 changed with the late run")
expect("${late}" "script: mission 3 failed at tick 233\n" "mission 3 did not fail on its time limit")
expect_not("${late}" "mission 3 complete" "the late run delivered")

# 3. Save in the middle of mission 3 (tick 120), then resume and finish the walk (80 more ticks of Move east).
file(WRITE "${WORK_DIR}/resume.input" "1-80 Move=1,0\n")
foreach(copy a b)
    play(part -ticks 120 -input "${INPUT}" -save mid -save_dir "${WORK_DIR}/saves_${copy}")
    expect("${part}" "script: mission 3 active at tick 83\n" "the save was not taken inside mission 3")
    expect_not("${part}" "mission 3 complete" "mission 3 was already done at the save")
    expect("${part}" "save 'mid' written \\(tick 120\\)" "no save written")
    play(resumed_${copy} -ticks 180 -input "${WORK_DIR}/resume.input" -save mid -save_dir "${WORK_DIR}/saves_${copy}")
    expect("${resumed_${copy}}" "save 'mid' loaded \\(12 entities, tick 120\\)" "the save did not load")
    expect("${resumed_${copy}}" "script: mission 3 complete at tick 176\n" "the resumed delivery completed at another tick")
    expect_not("${resumed_${copy}}" "mission [12] (active|complete)" "a finished mission started again after the load")
    expect_not("${resumed_${copy}}" "failed at|\\[error\\]|\\[warn\\]" "the resumed run logged a problem")
endforeach()
hash_of("${resumed_a}" hash_ra)
hash_of("${resumed_b}" hash_rb)
if(NOT hash_ra STREQUAL hash_rb)
    message(FATAL_ERROR "two resumes differ: ${hash_ra} vs ${hash_rb}")
endif()

message(STATUS "10_missions holds: reach 58, kill 82, deliver 176, late fail 233, save/load mid-mission, state_hash ${hash_a} (twice)")
