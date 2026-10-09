# mce_cook --module contract test (run with cmake -P): a project whose scenes use a game module's
# components cooks only with that module loaded. Exit 1 (unknown component) without it, 0 with it;
# a module that cannot be loaded is exit 1 with a diagnostic (the input is wrong, not the usage).

function(expect_exit expected)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL expected)
        message(FATAL_ERROR "expected exit ${expected}, got ${rc}: ${ARGN}\n${out}\n${err}")
    endif()
    set(LAST_OUTPUT "${out}${err}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/project")
file(COPY "${PROJECT_DIR}/assets" DESTINATION "${WORK_DIR}/project")

expect_exit(2 "${MCE_COOK}" --module)
expect_exit(1 "${MCE_COOK}" --module "${WORK_DIR}/no_such_module.so" "${WORK_DIR}/project")
if(NOT LAST_OUTPUT MATCHES "cannot load game module")
    message(FATAL_ERROR "missing module diagnostic:\n${LAST_OUTPUT}")
endif()

# Engine components only: the game's components are unknown.
expect_exit(1 "${MCE_COOK}" "${WORK_DIR}/project" --quiet)
if(NOT LAST_OUTPUT MATCHES "Spin|Bob")
    message(FATAL_ERROR "expected an unknown-component diagnostic naming the game's component:\n${LAST_OUTPUT}")
endif()

# With the module: everything cooks, and the pack is byte-identical to a second cook.
expect_exit(0 "${MCE_COOK}" --module "${MODULE}" "${WORK_DIR}/project" --pack --force)
if(NOT LAST_OUTPUT MATCHES "0 failed")
    message(FATAL_ERROR "cook with the module failed:\n${LAST_OUTPUT}")
endif()
file(SHA256 "${WORK_DIR}/project/assets.mce" pack_a)
expect_exit(0 "${MCE_COOK}" --module "${MODULE}" "${WORK_DIR}/project" --pack --force)
file(SHA256 "${WORK_DIR}/project/assets.mce" pack_b)
if(NOT pack_a STREQUAL pack_b)
    message(FATAL_ERROR "cook with a module is not deterministic: ${pack_a} vs ${pack_b}")
endif()
message(STATUS "mce_cook --module contract holds (pack ${pack_a})")
