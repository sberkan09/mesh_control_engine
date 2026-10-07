# mce_cook CLI contract test (run with cmake -P).
#   exit 2 on usage errors, 0 on a clean cook, 1 on cook errors;
#   two from-scratch cooks of the sample project produce byte-identical pack + manifest.

function(expect_exit expected)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL expected)
        message(FATAL_ERROR "expected exit ${expected}, got ${rc}: ${ARGN}\n${out}\n${err}")
    endif()
    set(LAST_OUTPUT "${out}${err}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

expect_exit(2 "${MCE_COOK}")
expect_exit(2 "${MCE_COOK}" --bogus "${WORK_DIR}")
expect_exit(2 "${MCE_COOK}" a b)

foreach(run a b)
    file(COPY "${DATA_DIR}/sample_project" DESTINATION "${WORK_DIR}/${run}")
    expect_exit(0 "${MCE_COOK}" "${WORK_DIR}/${run}/sample_project" --pack)
    file(SHA256 "${WORK_DIR}/${run}/sample_project/assets.mce" pack_${run})
    file(SHA256 "${WORK_DIR}/${run}/sample_project/cooked/manifest.mce" manifest_${run})
endforeach()
if(NOT pack_a STREQUAL pack_b OR NOT manifest_a STREQUAL manifest_b)
    message(FATAL_ERROR "cook is not deterministic: pack ${pack_a} vs ${pack_b}, manifest ${manifest_a} vs ${manifest_b}")
endif()

# Incremental: nothing changed, nothing re-cooks, same bytes.
expect_exit(0 "${MCE_COOK}" "${WORK_DIR}/a/sample_project" --pack)
if(NOT LAST_OUTPUT MATCHES "0 cooked, 3 up to date")
    message(FATAL_ERROR "second cook was not incremental:\n${LAST_OUTPUT}")
endif()
file(SHA256 "${WORK_DIR}/a/sample_project/assets.mce" pack_again)
if(NOT pack_again STREQUAL pack_a)
    message(FATAL_ERROR "incremental cook changed the pack")
endif()

# Cook errors: exit 1 with a file:line:col diagnostic.
file(COPY "${DATA_DIR}/broken_project" DESTINATION "${WORK_DIR}")
expect_exit(1 "${MCE_COOK}" "${WORK_DIR}/broken_project" --quiet)
if(NOT LAST_OUTPUT MATCHES "assets/materials/broken.mcmat:5:[0-9]+: error: missing reference")
    message(FATAL_ERROR "missing file:line:col diagnostic:\n${LAST_OUTPUT}")
endif()

message(STATUS "mce_cook CLI contract holds (pack ${pack_a})")
