# MCE_ENGINE_ID (scripting.md §2.8): a 64-bit stamp of the engine build, compiled into the engine
# and into every game module built against it. The loader refuses a module whose stamp differs
# (Errc::EngineIdMismatch), which is how a stale module build is caught.
#
# Inputs: `git describe` at configure time + build type + compiler + sanitizer setting. Taken at
# configure time, so the stamp is constant within one build tree (engine and modules always agree
# when built together) and changes when the tree is reconfigured on another commit.

find_package(Git QUIET)
set(MCE_GIT_DESCRIBE "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --tags
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        OUTPUT_VARIABLE _mce_describe
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _mce_describe_rc)
    if(_mce_describe_rc EQUAL 0 AND _mce_describe)
        set(MCE_GIT_DESCRIBE "${_mce_describe}")
    endif()
endif()

set(MCE_ENGINE_DESCRIBE
    "${MCE_GIT_DESCRIBE} ${CMAKE_BUILD_TYPE} ${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION} asan=${MCE_ASAN}")
string(SHA256 _mce_engine_hash "${MCE_ENGINE_DESCRIBE}")
string(SUBSTRING "${_mce_engine_hash}" 0 16 MCE_ENGINE_ID_HEX)

set(MCE_GENERATED_INCLUDE_DIR "${PROJECT_BINARY_DIR}/generated")
configure_file("${CMAKE_CURRENT_LIST_DIR}/engine_version.h.in"
    "${MCE_GENERATED_INCLUDE_DIR}/mce/engine_version.h" @ONLY)
message(STATUS "MCE engine id: ${MCE_ENGINE_ID_HEX} (${MCE_ENGINE_DESCRIBE})")
