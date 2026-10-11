# SDK contract test (run with cmake -P, see CMakeLists.txt): install, new, build, headless, mcp, cook, ship.

function(expect_exit expected)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL expected)
        message(FATAL_ERROR "expected exit ${expected}, got ${rc}: ${ARGN}\n${out}\n${err}")
    endif()
    set(LAST_OUTPUT "${out}${err}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(PREFIX "${WORK_DIR}/sdk")
set(MCE "${PYTHON}" "${PREFIX}/bin/mce")

# 1. Install this build tree as an SDK.
expect_exit(0 ${CMAKE_COMMAND} --install "${BUILD_DIR}" --prefix "${PREFIX}")
foreach(f bin/mce bin/mce_player bin/mce_editor bin/mce_mcp bin/mce_cook lib/cmake/MCE/MCEConfig.cmake
          include/mce/gameplay.h include/mce/engine_version.h include/spdlog/spdlog.h include/mce/net/net.h
          share/mce/templates/project/CMakeLists.txt share/mce/templates/project/assets/fonts/ProggyClean.ttf
          share/mce/scripts/new_project.py share/mce/VERSION share/mce/examples/Kindle/CMakeLists.txt
          share/doc/mce/user-guide/README.md share/doc/mce/engine/modules.md share/doc/mce/ARCHITECTURE.md)
    if(NOT EXISTS "${PREFIX}/${f}")
        message(FATAL_ERROR "the SDK lacks ${f}")
    endif()
endforeach()
file(GLOB engine_lib "${PREFIX}/lib/*mce_engine*")
if(NOT engine_lib)
    message(FATAL_ERROR "the SDK lacks lib/libmce_engine")
endif()
expect_exit(0 ${MCE} version)
if(NOT LAST_OUTPUT MATCHES "^mce [0-9]")
    message(FATAL_ERROR "mce version:\n${LAST_OUTPUT}")
endif()
expect_exit(0 ${MCE} --help)
expect_exit(2 ${MCE})
# The installed tools run from the SDK (libmce_engine found through ../lib).
expect_exit(0 "${PREFIX}/bin/mce_player" -help)
expect_exit(0 "${PREFIX}/bin/mce_cook" --help)

# 2. A project from the template, built against the SDK with the engine's own compiler.
expect_exit(0 ${MCE} new SdkProbe --dest "${WORK_DIR}" --game sdk_probe)
set(project "${WORK_DIR}/SdkProbe")
set(defines "-DCMAKE_CXX_COMPILER=${CXX}" "-DCMAKE_C_COMPILER=${CC}")
if(LAUNCHER)
    list(APPEND defines "-DCMAKE_CXX_COMPILER_LAUNCHER=${LAUNCHER}" "-DCMAKE_C_COMPILER_LAUNCHER=${LAUNCHER}")
endif()
if(NOT CONFIG)
    set(CONFIG Debug)
endif()
expect_exit(0 ${MCE} build "${project}" --config "${CONFIG}" ${defines})
if(NOT EXISTS "${project}/build/project/assets.mce")
    message(FATAL_ERROR "mce build did not cook the pack:\n${LAST_OUTPUT}")
endif()

# 3. It plays (the module loaded by the host from the SDK's engine), cooks and ships.
expect_exit(0 ${MCE} headless "${project}" --ticks 300)
if(NOT LAST_OUTPUT MATCHES "300 ticks, [0-9]+ entities, state_hash")
    message(FATAL_ERROR "mce headless:\n${LAST_OUTPUT}")
endif()
# `mce mcp` speaks the MCP stdio transport: stdout carries JSON-RPC lines only, from the first byte.
file(WRITE "${WORK_DIR}/mcp_in.jsonl"
     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"clientInfo\":{\"name\":\"sdk_test\",\"version\":\"0\"}}}\n")
execute_process(COMMAND ${MCE} mcp "${project}" INPUT_FILE "${WORK_DIR}/mcp_in.jsonl"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "^{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":")
    message(FATAL_ERROR "mce mcp: stdout is not the initialize response alone (rc ${rc}):\n${out}\n${err}")
endif()
expect_exit(0 ${MCE} cook "${project}" --pack --force)
if(NOT LAST_OUTPUT MATCHES "0 failed")
    message(FATAL_ERROR "mce cook:\n${LAST_OUTPUT}")
endif()
expect_exit(0 ${MCE} ship "${project}" --version 1.2.3)
file(GLOB zips "${project}/dist/SdkProbe-1.2.3-*.zip")
if(NOT zips)
    message(FATAL_ERROR "mce ship wrote no zip:\n${LAST_OUTPUT}")
endif()
# The shipped folder runs on its own: the host finds the module, the engine and the pack beside it.
file(ARCHIVE_EXTRACT INPUT ${zips} DESTINATION "${WORK_DIR}/shipped")
file(GLOB shipped_host "${WORK_DIR}/shipped/SdkProbe/SdkProbe*")
list(FILTER shipped_host EXCLUDE REGEX "\\.(so|dylib|dll|mce|txt)$")
list(GET shipped_host 0 shipped_host)
expect_exit(0 "${shipped_host}" -headless 1 -ticks 300)
if(NOT LAST_OUTPUT MATCHES "300 ticks, [0-9]+ entities, state_hash")
    message(FATAL_ERROR "the shipped host:\n${LAST_OUTPUT}")
endif()
# 3b. The project opts into the networking module (ADR-0035): DEPS mce::mce_net links the SDK's
# libmce_net into the module, whose engine calls resolve against the SDK's libmce_engine.
file(GLOB net_lib "${PREFIX}/lib/*mce_net*")
if(net_lib)
    set(module_dir "${project}/games/sdk_probe/module")
    file(WRITE "${module_dir}/net_probe.cpp" [=[
#include "mce/net/net.h"

namespace sdk_probe {
bool net_probe() {
    mce::net::ByteWriter w;
    w.write_varu32(300);
    mce::net::ByteReader r(w.data());
    mce::net::Host host;
    const bool opened = host.open_client();
    host.close();
    return opened && r.read_varu32() == 300 && r.ok();
}
}  // namespace sdk_probe
]=])
    file(READ "${module_dir}/sdk_probe.cpp" game_src)
    string(REPLACE "ResultV install(World& world) {"
                   "bool net_probe();\nResultV install(World& world) {\n    std::printf(\"net probe %s\\n\", net_probe() ? \"ok\" : \"failed\");"
                   game_src "${game_src}")
    file(WRITE "${module_dir}/sdk_probe.cpp" "#include <cstdio>\n${game_src}")
    file(READ "${project}/CMakeLists.txt" project_cmake)
    string(REPLACE "GAME_SOURCES games/sdk_probe/module/sdk_probe.cpp"
                   "GAME_SOURCES games/sdk_probe/module/sdk_probe.cpp games/sdk_probe/module/net_probe.cpp"
                   project_cmake "${project_cmake}")
    string(REPLACE "INCLUDE_DIRS games/sdk_probe/module)" "INCLUDE_DIRS games/sdk_probe/module\n    DEPS mce::mce_net)"
                   project_cmake "${project_cmake}")
    file(WRITE "${project}/CMakeLists.txt" "${project_cmake}")
    expect_exit(0 ${MCE} build "${project}" --config "${CONFIG}" ${defines})
    expect_exit(0 ${MCE} headless "${project}" --ticks 300)
    if(NOT LAST_OUTPUT MATCHES "net probe ok" OR NOT LAST_OUTPUT MATCHES "300 ticks, [0-9]+ entities, state_hash")
        message(FATAL_ERROR "mce headless with mce_net:\n${LAST_OUTPUT}")
    endif()
endif()
# 4. The example project copies out of the SDK and builds against it the same way.
expect_exit(0 ${MCE} example)
if(NOT LAST_OUTPUT MATCHES "Kindle" OR NOT LAST_OUTPUT MATCHES "Undelivered")
    message(FATAL_ERROR "mce example lists no Kindle or no Undelivered:\n${LAST_OUTPUT}")
endif()
expect_exit(0 ${MCE} example Kindle --dest "${WORK_DIR}")
set(example "${WORK_DIR}/Kindle")
if(EXISTS "${example}/cooked" OR EXISTS "${example}/assets.mce")
    message(FATAL_ERROR "the example carries build outputs")
endif()
expect_exit(0 ${MCE} build "${example}" --config "${CONFIG}" ${defines})
expect_exit(0 ${MCE} headless "${example}" --ticks 300)
if(NOT LAST_OUTPUT MATCHES "300 ticks, [0-9]+ entities, state_hash")
    message(FATAL_ERROR "mce headless (Kindle):\n${LAST_OUTPUT}")
endif()
message(STATUS "SDK contract holds: install, new, build, headless, mcp, cook, ship, net, example")
