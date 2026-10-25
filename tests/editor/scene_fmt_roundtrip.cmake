# M-SCENEFMT: a scene with a default field spelled out is not canonical; --write fixes it, a second --write changes
# nothing, and --check passes afterwards. Run as: cmake -DFMT=<mce_scene_fmt> -DSRC=<scene> -DWORK=<dir> -P this.
file(MAKE_DIRECTORY "${WORK}")
set(copy "${WORK}/fmt_loose.mcscene")
file(COPY_FILE "${SRC}" "${copy}")

execute_process(COMMAND "${FMT}" "${copy}" RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 1 OR NOT out MATCHES "not canonical")
    message(FATAL_ERROR "the loose scene should be reported (rc=${rc}): ${out}")
endif()

execute_process(COMMAND "${FMT}" --write "${copy}" RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 0 OR NOT out MATCHES "formatted")
    message(FATAL_ERROR "--write should rewrite it (rc=${rc}): ${out}")
endif()

execute_process(COMMAND "${FMT}" "${copy}" RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "")
    message(FATAL_ERROR "the rewritten scene should be canonical (rc=${rc}): ${out}")
endif()

execute_process(COMMAND "${FMT}" --write "${copy}" RESULT_VARIABLE rc OUTPUT_VARIABLE out)
if(NOT rc EQUAL 0 OR NOT out STREQUAL "")
    message(FATAL_ERROR "a second --write should change nothing (rc=${rc}): ${out}")
endif()
