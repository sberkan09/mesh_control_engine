# 12_loot's headless gate (M-INVENTORY step 5), run with cmake -P: mce_player plays the cooked pack for 60 ticks and the
# script's "inv:" log lines pin the run (the script's tick counts from 0; events it queues land a few ticks later).
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
play(first -ticks 60 -input "${INPUT}")
play(second -ticks 60 -input "${INPUT}")
expect("${first}" "60 ticks, 7 entities, state_hash" "the run did not end after 60 ticks with 7 entities")
expect_not("${first}" "\\[error\\]|\\[warn\\]|does not load|failed at" "the run logged a problem")
# Gives: 7 potions (5 + 2), the sword and 30 coins weigh 7 * 0.5 + 3 = 6.5 kg.
expect("${first}" "script: inv: given coin 30 potion 7 sword 1 gem 0 weight 6500\n" "the items did not arrive")
expect("${first}" "script: inv: slot 1 is potion x5\n" "the first stack is not 5 potions")
# The sword is worn (out of the slots, so 3 kg), one potion is drunk.
expect("${first}" "script: inv: equipped and drank coin 30 potion 6 sword 0 gem 0 weight 3000\n" "equip or use did not land")
# M-ITEMFX: the hero starts at 40/100 health; the potion heals 25 and the helm adds 20 max health and 0.2 armor.
expect("${first}" "script: inv: health 65/120 armor 0.2\n" "the potion did not heal or the helm did not armour")
# The chest rolls its table (seed 7) on the first open: 32 coins in this run.
expect("${first}" "script: inv: opened chest coin 62 potion 6 sword 0 gem 0 weight 3000\n" "the chest did not hand over its loot")
# The goblin (seed 11) dies: its table is one gem plus one roll, as pickups.
expect("${first}" "script: inv: goblin dropped 2 pickups\n" "the goblin did not drop its table")
expect("${first}" "script: inv: dropped 2 potions\n" "drop did not take two potions")
expect("${first}" "script: inv: dropped coin 62 potion 4 sword 0 gem 0 weight 2000\n" "the dropped potions are still carried")
# M-CRAFT: the tonic is made by hand (1 potion + 5 coin); the pack needs the workbench (3 potions, 0.2 s) and is not craftable by hand.
expect("${first}" "script: inv: can tonic true, pack by hand false, pack at bench true\n" "can_craft disagrees with the recipes")
expect("${first}" "script: inv: crafted tonic coin 57 potion 3 sword 0 gem 0 weight 1700\n" "the hand craft did not land")
expect("${first}" "script: inv: tonic 1 potion_pack 1\n" "the workbench craft did not finish")
expect("${first}" "script: inv: crafted pack coin 57 potion 0 sword 0 gem 0 weight 1400\n" "the pack did not take its potions")
hash_of("${first}" hash_a)
hash_of("${second}" hash_b)
if(NOT hash_a STREQUAL hash_b)
    message(FATAL_ERROR "two plays differ: ${hash_a} vs ${hash_b}")
endif()
message(STATUS "12_loot holds: 62 coins from the chest, 2 goblin pickups, state_hash ${hash_a} (twice)")
