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
          share/mce/templates/novel/story/story.json share/mce/scripts/story_compile.py include/mce/kit/novel.h
          include/mce/kit/novel_ui.h
          share/mce/scripts/new_project.py share/mce/VERSION share/mce/examples/Kindle/CMakeLists.txt
          share/doc/mce/user-guide/README.md share/doc/mce/engine/modules.md share/doc/mce/ARCHITECTURE.md
          share/mce/samples/03_platformer/main.cpp share/mce/samples/scene_demo/assets/scenes/demo.mcscene
          share/mce/library/library.json)
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
# A custom host names engine libraries as in the engine tree; mce_engine_deps maps them to the umbrella (#124).
file(APPEND "${project}/CMakeLists.txt" [=[
mce_engine_deps(sdk_probe_host_deps mce::mce_render mce::mce_physics3d mce_audio)
if(NOT sdk_probe_host_deps STREQUAL "mce::mce_engine")
    message(FATAL_ERROR "mce_engine_deps against the SDK gave '${sdk_probe_host_deps}'")
endif()
target_link_libraries(sdk_probe PRIVATE ${sdk_probe_host_deps})
]=])
expect_exit(0 ${MCE} build "${project}" --config "${CONFIG}" ${defines})
if(NOT EXISTS "${project}/build/project/assets.mce")
    message(FATAL_ERROR "mce build did not cook the pack:\n${LAST_OUTPUT}")
endif()

# 3. It plays (the module loaded by the host from the SDK's engine), cooks and ships.
expect_exit(0 ${MCE} headless "${project}" --ticks 300)
if(NOT LAST_OUTPUT MATCHES "300 ticks, [0-9]+ entities, state_hash")
    message(FATAL_ERROR "mce headless:\n${LAST_OUTPUT}")
endif()
# The module's log lines go through the host's logger (the engine's format, no file:line), so the
# in-game console and the MCP runs' log tails show them.
if(NOT LAST_OUTPUT MATCHES "[0-9]\\] \\[info\\] sdk_probe: game systems installed" OR LAST_OUTPUT MATCHES "\\[sdk_probe.cpp:")
    message(FATAL_ERROR "mce headless: the module's log line is not in the host's log:\n${LAST_OUTPUT}")
endif()
# `mce mcp` speaks the MCP stdio transport: stdout carries JSON-RPC lines only, from the first byte.
file(WRITE "${WORK_DIR}/mcp_in.jsonl"
     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"clientInfo\":{\"name\":\"sdk_test\",\"version\":\"0\"}}}\n")
execute_process(COMMAND ${MCE} mcp "${project}" INPUT_FILE "${WORK_DIR}/mcp_in.jsonl"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "^{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":")
    message(FATAL_ERROR "mce mcp: stdout is not the initialize response alone (rc ${rc}):\n${out}\n${err}")
endif()
# The SDK's server serves the SDK's own docs and samples (mce://docs/..., docs_search), not the
# engine source tree's.
file(WRITE "${WORK_DIR}/mcp_docs.jsonl"
     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{\"name\":\"sdk_test\",\"version\":\"0\"}}}\n"
     "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
     "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"resources/read\",\"params\":{\"uri\":\"mce://docs/guide/14-mcp-ai-workflow.md\"}}\n"
     "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"docs_search\",\"arguments\":{\"query\":\"platformer jump\",\"group\":\"samples\"}}}\n"
     "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"library_search\",\"arguments\":{\"query\":\"handwriting\"}}}\n")
execute_process(COMMAND "${PREFIX}/bin/mce_mcp" INPUT_FILE "${WORK_DIR}/mcp_docs.jsonl"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "Working with an AI agent" OR NOT out MATCHES "mce://docs/samples/03_platformer/"
   OR NOT out MATCHES "font-nothing-you-could-do")
    message(FATAL_ERROR "the SDK's mce_mcp does not serve its docs and content library (rc ${rc}):\n${out}\n${err}")
endif()
# `mce new` registered that server for Claude Code in <project>/.mcp.json; the entry starts it from anywhere.
file(READ "${project}/.mcp.json" mcp_json)
string(JSON mcp_cmd GET "${mcp_json}" mcpServers mce_sdk_probe command)
string(JSON mcp_arg0 GET "${mcp_json}" mcpServers mce_sdk_probe args 0)
string(JSON mcp_arg1 GET "${mcp_json}" mcpServers mce_sdk_probe args 1)
execute_process(COMMAND "${mcp_cmd}" "${mcp_arg0}" "${mcp_arg1}" INPUT_FILE "${WORK_DIR}/mcp_in.jsonl"
                WORKING_DIRECTORY "${WORK_DIR}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "^{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":")
    message(FATAL_ERROR "the .mcp.json entry does not start the server (rc ${rc}):\n${mcp_json}\n${out}\n${err}")
endif()
# mcp-config --desktop merges the same entry into Claude Desktop's config, keeping other servers.
set(ENV{MCE_CLAUDE_DESKTOP_CONFIG} "${WORK_DIR}/claude_desktop_config.json")
file(WRITE "${WORK_DIR}/claude_desktop_config.json" "{\"mcpServers\":{\"other\":{\"command\":\"x\"}}}")
expect_exit(0 ${MCE} mcp-config "${project}" --desktop)
file(READ "${WORK_DIR}/claude_desktop_config.json" desktop_json)
string(JSON other_cmd ERROR_VARIABLE jerr GET "${desktop_json}" mcpServers other command)
string(JSON probe_cmd ERROR_VARIABLE jerr2 GET "${desktop_json}" mcpServers mce_sdk_probe command)
if(NOT other_cmd STREQUAL "x" OR NOT probe_cmd STREQUAL mcp_cmd)
    message(FATAL_ERROR "mce mcp-config --desktop:\n${desktop_json}")
endif()
unset(ENV{MCE_CLAUDE_DESKTOP_CONFIG})
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
# A fresh project registers its smoke test, so ctest (and the MCP tool module_test) has one to run.
expect_exit(0 ${CMAKE_CTEST_COMMAND} --test-dir "${project}/build" -R headless_300)
if(NOT LAST_OUTPUT MATCHES "100% tests passed, 0 tests failed out of 1")
    message(FATAL_ERROR "the project's smoke test:\n${LAST_OUTPUT}")
endif()
if(NOT EXISTS "${WORK_DIR}/shipped/SdkProbe/CREDITS.md")
    message(FATAL_ERROR "mce ship left out CREDITS.md")
endif()
# The agent's path to the same zip (M-AGENT step 6): project_check, then ship_build (build already done).
file(WRITE "${WORK_DIR}/mcp_ship.jsonl"
     "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{\"name\":\"sdk_test\",\"version\":\"0\"}}}\n"
     "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
     "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"project_check\",\"arguments\":{\"ticks\":30}}}\n"
     "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"ship_build\",\"arguments\":{\"build\":false,\"ticks\":30,\"version\":\"9.9.9\"}}}\n"
     "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"report_friction\",\"arguments\":{\"summary\":\"sdk probe note\",\"area\":\"ship_build\",\"severity\":\"minor\"}}}\n")
execute_process(COMMAND ${MCE} mcp "${project}" INPUT_FILE "${WORK_DIR}/mcp_ship.jsonl"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
file(GLOB agent_zips "${project}/dist/SdkProbe-9.9.9-*.zip")
if(NOT rc EQUAL 0 OR NOT out MATCHES "pass[^a-z]+true" OR NOT agent_zips)
    message(FATAL_ERROR "project_check / ship_build over MCP (rc ${rc}):\n${out}\n${err}")
endif()
# The note report_friction wrote, collected for the engine's backlog (M-AGENT step 8).
expect_exit(0 ${MCE} friction "${project}" --out "${WORK_DIR}/friction.md")
file(READ "${WORK_DIR}/friction.md" backlog)
if(NOT backlog MATCHES "### ship_build \\(1\\)\n\n- sdk probe note \\(SdkProbe, sdk_test-")
    message(FATAL_ERROR "mce friction:\n${backlog}")
endif()
# 3a. A new asset without a sidecar stops the pack before the host links; `mce cook --adopt` still runs
# (it needs only the module) and the next build succeeds (#117).
file(REMOVE "${project}/build/sdk_probe" "${project}/build/sdk_probe.exe")
file(COPY_FILE "${project}/assets/fonts/ProggyClean.ttf" "${project}/assets/fonts/Adopted.ttf")
expect_exit(1 ${MCE} build "${project}" --config "${CONFIG}" ${defines})
if(NOT LAST_OUTPUT MATCHES "--adopt")
    message(FATAL_ERROR "mce build gave no adopt hint:\n${LAST_OUTPUT}")
endif()
expect_exit(0 ${MCE} cook "${project}" --adopt)
if(NOT EXISTS "${project}/assets/fonts/Adopted.ttf.mceasset")
    message(FATAL_ERROR "mce cook --adopt wrote no sidecar:\n${LAST_OUTPUT}")
endif()
expect_exit(0 ${MCE} build "${project}" --config "${CONFIG}" ${defines})
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
if(NOT LAST_OUTPUT MATCHES "Kindle" OR NOT LAST_OUTPUT MATCHES "Undelivered" OR NOT LAST_OUTPUT MATCHES "Room3D")
    message(FATAL_ERROR "mce example lists no Kindle, Undelivered or Room3D:\n${LAST_OUTPUT}")
endif()
expect_exit(0 ${MCE} example Kindle --dest "${WORK_DIR}")
set(example "${WORK_DIR}/Kindle")
if(EXISTS "${example}/cooked" OR EXISTS "${example}/assets.mce")
    message(FATAL_ERROR "the example carries build outputs")
endif()
if(NOT EXISTS "${example}/.mcp.json")
    message(FATAL_ERROR "mce example wrote no .mcp.json")
endif()
expect_exit(0 ${MCE} build "${example}" --config "${CONFIG}" ${defines})
expect_exit(0 ${MCE} headless "${example}" --ticks 300)
if(NOT LAST_OUTPUT MATCHES "300 ticks, [0-9]+ entities, state_hash")
    message(FATAL_ERROR "mce headless (Kindle):\n${LAST_OUTPUT}")
endif()
# 5. A visual novel from the novel template (mce_kit_novel): the build compiles its story, the bot
# plays it to @end headless, and it ships.
expect_exit(0 ${MCE} new NovelSdk --dest "${WORK_DIR}" --template novel)
set(novel "${WORK_DIR}/NovelSdk")
expect_exit(0 ${MCE} story compile "${novel}" --check --werror)
expect_exit(0 ${MCE} build "${novel}" --config "${CONFIG}" ${defines})
expect_exit(0 ${MCE} headless "${novel}" --ticks 2000 -- -novel.autoplay 1)
if(NOT LAST_OUTPUT MATCHES "novel: @end" OR NOT LAST_OUTPUT MATCHES "2000 ticks, [0-9]+ entities, state_hash")
    message(FATAL_ERROR "mce headless (novel template):\n${LAST_OUTPUT}")
endif()
expect_exit(0 ${MCE} ship "${novel}" --version 0.1.0)
file(GLOB novel_zips "${novel}/dist/NovelSdk-0.1.0-*.zip")
if(NOT novel_zips)
    message(FATAL_ERROR "mce ship (novel template) wrote no zip:\n${LAST_OUTPUT}")
endif()
# 5a. Its host adds the NPC dialogue box (mce_kit_novel_ui, HOST_DEPS) as a player UI layer.
file(READ "${novel}/games/novel_sdk/host/main.cpp" host_src)
string(REPLACE "#include \"mce/player/player.h\"" "#include \"mce/player/player.h\"\n#include \"mce/kit/novel_ui.h\""
               host_src "${host_src}")
string(REPLACE "    return mce::player::run(" "    mce::novel::DialogueBox box;\n    const mce::ui::Layer* layers[] = {box.layer()};\n    desc.ui_layers = layers;\n    desc.ui_layer_count = 1;\n    return mce::player::run("
               host_src "${host_src}")
file(WRITE "${novel}/games/novel_sdk/host/main.cpp" "${host_src}")
file(READ "${novel}/CMakeLists.txt" novel_cmake)
string(REPLACE "DEPS mce::mce_kit_novel)" "DEPS mce::mce_kit_novel\n    HOST_DEPS mce::mce_kit_novel_ui)" novel_cmake "${novel_cmake}")
file(WRITE "${novel}/CMakeLists.txt" "${novel_cmake}")
expect_exit(0 ${MCE} build "${novel}" --config "${CONFIG}" ${defines})
expect_exit(0 ${MCE} headless "${novel}" --ticks 2000 -- -novel.autoplay 1)
if(NOT LAST_OUTPUT MATCHES "novel: @end")
    message(FATAL_ERROR "mce headless (novel with the dialogue box):\n${LAST_OUTPUT}")
endif()
# 6. The 3D example (M-KIT3D): only its module, on the kit, the stock player's 3D path; the cook
# builds its rocks' LOD chains with the SDK's cook tool.
expect_exit(0 ${MCE} example Room3D --dest "${WORK_DIR}")
set(example3d "${WORK_DIR}/Room3D")
expect_exit(0 ${MCE} build "${example3d}" --config "${CONFIG}" ${defines})
expect_exit(0 ${MCE} headless "${example3d}" --ticks 300)
if(NOT LAST_OUTPUT MATCHES "300 ticks, [0-9]+ entities, state_hash")
    message(FATAL_ERROR "mce headless (Room3D):\n${LAST_OUTPUT}")
endif()
message(STATUS "SDK contract holds: install, new, build, headless, mcp, cook, ship, net, example, novel, 3D example")
