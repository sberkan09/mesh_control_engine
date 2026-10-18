# Scene scripts end to end (M-SCRIPT 1), run with cmake -P: a project with a .luau script and a
# scene naming it in a ScriptSystem cooks to a pack; mce_player -headless runs it from bytecode
# (the player has no Luau compiler); a script that errors is reported with its file and line and
# the scene keeps running; a broken source fails the cook at file:line:col.

function(run expected_rc)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL expected_rc)
        message(FATAL_ERROR "expected exit ${expected_rc}, got ${rc}: ${ARGN}\n${out}\n${err}")
    endif()
    set(LAST_OUTPUT "${out}${err}" PARENT_SCOPE)
endfunction()

function(sidecar path guid)
    file(WRITE "${path}.mceasset" "{ \"guid\": \"${guid}\", \"importer\": \"script\", \"importer_version\": 2, \"settings\": {} }\n")
endfunction()

set(SCENE_GUID "5c000000-0000-4000-8000-0000000000a1")
set(PUSH_GUID "5c000000-0000-4000-8000-0000000000a2")
set(FAIL_GUID "5c000000-0000-4000-8000-0000000000a3")
file(REMOVE_RECURSE "${WORK_DIR}")

file(WRITE "${WORK_DIR}/game/assets/scripts/push.luau" [=[
-- Moves the crate one unit a tick and reports where it is at tick 10 (the 11th: ticks count from 0).
function update()
    mce.each("LocalTransform", function(e)
        local p = mce.get(e, "LocalTransform", "position") + vector.create(1, 0, 0)
        mce.set(e, "LocalTransform", "position", p)
        if mce.tick() == 10 then mce.log("crate x=" .. p.x) end
    end)
end
]=])
sidecar("${WORK_DIR}/game/assets/scripts/push.luau" ${PUSH_GUID})
file(WRITE "${WORK_DIR}/game/assets/scripts/fail.luau" [=[
function update()
    local t = nil
    t.boom = 1
end
]=])
sidecar("${WORK_DIR}/game/assets/scripts/fail.luau" ${FAIL_GUID})
file(WRITE "${WORK_DIR}/game/assets/scenes/main.mcscene" "{
  \"format\": \"mce-scene\", \"version\": 1, \"guid\": \"${SCENE_GUID}\", \"name\": \"main\",
  \"entities\": [
    { \"name\": \"Crate\", \"components\": { \"LocalTransform\": { \"position\": [0, 0, 0] } } },
    { \"name\": \"Script: fail\", \"components\": { \"ScriptSystem\": { \"script\": \"guid:${FAIL_GUID}\", \"order\": 0 } } },
    { \"name\": \"Script: push\", \"components\": { \"ScriptSystem\": { \"script\": \"guid:${PUSH_GUID}\", \"order\": 1, \"phase\": \"Post\" } } }
  ]
}
")
run(0 "${MCE_COOK}" "${WORK_DIR}/game" --pack --quiet)
# The API the scripts were checked against, for editors (M-SCRIPT 4).
file(READ "${WORK_DIR}/game/cooked/mce.d.luau" defs)
if(NOT defs MATCHES "declare mce: {" OR NOT defs MATCHES "\"LocalTransform\"")
    message(FATAL_ERROR "the cook did not write cooked/mce.d.luau:\n${defs}")
endif()

set(PLAY "${MCE_PLAYER}" -pack "${WORK_DIR}/game/assets.mce" -scene ${SCENE_GUID} -headless 1 -ticks 12 -platform.error_box 0)
# A script that failed makes the headless run exit 3 (M-SCRIPT3 2b), after printing the state hash.
run(3 ${PLAY})
if(NOT LAST_OUTPUT MATCHES "script [^:\n]*: crate x=11")
    message(FATAL_ERROR "the scene's script did not run in the player:\n${LAST_OUTPUT}")
endif()
if(NOT LAST_OUTPUT MATCHES "scripts/fail.luau:3: ")
    message(FATAL_ERROR "the failing script was not reported with its line:\n${LAST_OUTPUT}")
endif()
string(REGEX MATCH "state_hash[^\n]*" first "${LAST_OUTPUT}")
run(3 ${PLAY})
string(REGEX MATCH "state_hash[^\n]*" second "${LAST_OUTPUT}")
if(first STREQUAL "" OR NOT first STREQUAL second)
    message(FATAL_ERROR "two runs differ: '${first}' vs '${second}'")
endif()

# Hidden state the cook's static check misses (a top-level table passed to a helper) fails at run time (M-SCRIPT3 2): a headless run checks by
# default, and -script.state_check 0 turns it off.
file(WRITE "${WORK_DIR}/game/assets/scripts/push.luau" "local t = { n = 0 }\nlocal function bump(x)\n    x.n += 1\nend\nfunction update() bump(t) end\n")
run(0 "${MCE_COOK}" "${WORK_DIR}/game" --pack --quiet)
run(3 ${PLAY})
if(NOT LAST_OUTPUT MATCHES "push.luau:3: [^\n]*readonly table")
    message(FATAL_ERROR "the run-time state check did not fail the helper script at its line:\n${LAST_OUTPUT}")
endif()
run(3 ${PLAY} -script.state_check 0)
if(LAST_OUTPUT MATCHES "readonly table")
    message(FATAL_ERROR "-script.state_check 0 still froze the tables:\n${LAST_OUTPUT}")
endif()

# A syntax error fails the cook with the file, line and column.
file(WRITE "${WORK_DIR}/game/assets/scripts/push.luau" "function update()\n  local x = = 1\nend\n")
run(1 "${MCE_COOK}" "${WORK_DIR}/game" --pack)
if(NOT LAST_OUTPUT MATCHES "push.luau:2:13")
    message(FATAL_ERROR "the cook error does not name file:line:col:\n${LAST_OUTPUT}")
endif()

# State kept outside components fails the cook too (M-SCRIPT 3).
file(WRITE "${WORK_DIR}/game/assets/scripts/push.luau" "local count = 0\nfunction update()\n    count += 1\nend\n")
run(1 "${MCE_COOK}" "${WORK_DIR}/game" --pack)
if(NOT LAST_OUTPUT MATCHES "push.luau:3:5: [^\n]*top-level local 'count'")
    message(FATAL_ERROR "the cook does not reject hidden state at file:line:col:\n${LAST_OUTPUT}")
endif()

# ... while a local that points into a top-level table is rejected by the cook itself (M-SCRIPT3 2c).
file(WRITE "${WORK_DIR}/game/assets/scripts/push.luau" "local t = { n = 0 }\nfunction update()\n    local a = t\n    a.n += 1\nend\n")
run(1 "${MCE_COOK}" "${WORK_DIR}/game" --pack)
if(NOT LAST_OUTPUT MATCHES "push.luau:4:5: [^\n]*points into, which belongs to top-level local 't'")
    message(FATAL_ERROR "the cook does not reject the alias at file:line:col:\n${LAST_OUTPUT}")
endif()

# Names are checked against the registry and every problem is reported, each at file:line:col.
file(WRITE "${WORK_DIR}/game/assets/scripts/push.luau"
     "function update()\n    mce.each(\"LocalTransfrom\", function(e) end)\n    print(mce.tick())\nend\n")
run(1 "${MCE_COOK}" "${WORK_DIR}/game" --pack)
if(NOT LAST_OUTPUT MATCHES "push.luau:2:14: [^\n]*unknown component 'LocalTransfrom' \\(did you mean 'LocalTransform'\\?\\)"
   OR NOT LAST_OUTPUT MATCHES "push.luau:3:5: [^\n]*'print' is not available")
    message(FATAL_ERROR "the cook does not report every name problem at file:line:col:\n${LAST_OUTPUT}")
endif()

message(STATUS "scene scripts hold: cooked bytecode runs in the player, errors name file and line")
