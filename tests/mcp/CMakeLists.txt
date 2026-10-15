# Editor MCP server (editor-mcp.md §6): in-process protocol/tool tests + the FR-EDT-160 gate + fuzz,
# a transcript test that drives the real mce_mcp binary over pipes against golden responses, and
# a symbol wall (no ImGui, no socket API in mce_mcp*).
mce_add_test(test_mcp UMBRELLA
    SOURCES
        test_mcp.cpp
    DEPS
        mce::mce_mcp_core
        mce::mce_module_host
        mce::mce_platform)
target_include_directories(test_mcp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../asset)
target_compile_definitions(test_mcp PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp"
    MCE_SOURCE_DIR="${PROJECT_SOURCE_DIR}" MCE_PLAYER_EXE="$<TARGET_FILE:mce_player>")
add_dependencies(test_mcp mce_player)
if(TARGET template_probe_game)
    # The stock server loads the template probe's game module (Server::load_module): its components
    # cook and its systems run in run_headless, hashing like the probe's own host.
    target_compile_definitions(test_mcp PRIVATE
        MCE_PROBE_MODULE="$<TARGET_FILE:template_probe_game>"
        MCE_PROBE_PROJECT="${CMAKE_BINARY_DIR}/template_probe/TemplateProbe"
        MCE_PROBE_HOST="$<TARGET_FILE:template_probe>")
    add_dependencies(test_mcp template_probe_game template_probe)
    set_tests_properties(test_mcp PROPERTIES RESOURCE_LOCK template_probe_module)
endif()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    # Re-bless after an intended protocol change:
    #   python3 tests/mcp/run_transcript.py --exe <build>/src/editor/mcp/mce_mcp --update
    add_test(NAME mcp_transcript
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_transcript.py
                     --exe $<TARGET_FILE:mce_mcp> --work ${CMAKE_CURRENT_BINARY_DIR}/transcript)
    if(CMAKE_NM AND NOT WIN32)
        add_test(NAME mcp_symbol_wall
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check_symbols.py --nm ${CMAKE_NM}
                         --no-sockets $<TARGET_FILE:mce_mcp_core>
                         --no-imgui $<TARGET_FILE:mce_mcp_core> $<TARGET_FILE:mce_mcp>)
    endif()
endif()

# The M-AGENT tools (render_frame, render_sheet, image_diff, ...). The "gpu" suite draws with the
# real player and registers only with MCE_GPU_TESTS (CI: Xvfb + Mesa llvmpipe).
mce_add_test(test_mcp_agent UMBRELLA NO_CTEST
    SOURCES
        test_mcp_agent.cpp
        test_mcp_ui.cpp
    DEPS
        mce::mce_mcp_core
        mce::mce_module_host
        mce::mce_platform)
target_include_directories(test_mcp_agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../asset)
target_compile_definitions(test_mcp_agent PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp_agent"
    MCE_SOURCE_DIR="${PROJECT_SOURCE_DIR}" MCE_PLAYER_EXE="$<TARGET_FILE:mce_player>")
add_dependencies(test_mcp_agent mce_player)
if(TARGET template_probe_game)
    target_compile_definitions(test_mcp_agent PRIVATE MCE_PROBE_MODULE="$<TARGET_FILE:template_probe_game>"
        MCE_PROBE_PROJECT="${CMAKE_BINARY_DIR}/template_probe/TemplateProbe")
    add_dependencies(test_mcp_agent template_probe_game)
endif()
add_test(NAME test_mcp_agent COMMAND test_mcp_agent -tse=gpu)
if(TARGET template_probe_game)
    set_tests_properties(test_mcp_agent PROPERTIES RESOURCE_LOCK template_probe_module)
endif()
if(MCE_GPU_TESTS)
    add_test(NAME test_mcp_agent_gpu COMMAND test_mcp_agent -ts=gpu)
    set_tests_properties(test_mcp_agent_gpu PROPERTIES LABELS gpu)
endif()

# test_run, playtest and run_headless's trace against a tiny game defined in the test.
mce_add_test(test_mcp_playtest UMBRELLA
    SOURCES
        test_mcp_playtest.cpp
    DEPS
        mce::mce_mcp_core
        mce::mce_module_host
        mce::mce_platform)
target_include_directories(test_mcp_playtest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../asset)
target_compile_definitions(test_mcp_playtest PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp_playtest")

# mce://docs resources, docs_search, prompts and engine_capabilities (served from the source tree).
mce_add_test(test_mcp_docs UMBRELLA
    SOURCES
        test_mcp_docs.cpp
    DEPS
        mce::mce_mcp_core
        mce::mce_platform)
target_include_directories(test_mcp_docs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../asset)
target_compile_definitions(test_mcp_docs PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp_docs")

# The content generators and the bundled library (served from the source tree's library/).
mce_add_test(test_mcp_content UMBRELLA
    SOURCES
        test_mcp_content.cpp
    DEPS
        mce::mce_mcp_core
        mce::mce_platform)
target_include_directories(test_mcp_content PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../asset)
target_compile_definitions(test_mcp_content PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp_content"
    MCE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# project_check against planted problems; ship_build's refusals (its zip is checked in tests/sdk).
mce_add_test(test_mcp_check UMBRELLA
    SOURCES
        test_mcp_check.cpp
    DEPS
        mce::mce_mcp_core
        mce::mce_platform)
target_include_directories(test_mcp_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../asset)
target_compile_definitions(test_mcp_check PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp_check"
    MCE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# Several agents on one project: scene merge, locks, tasks, the event feed, scene_switch. The
# merge's unit cases include the server's private tools_team.h.
mce_add_test(test_mcp_team UMBRELLA
    SOURCES
        test_mcp_team.cpp
    DEPS
        mce::mce_mcp_core
        mce::mce_platform)
target_include_directories(test_mcp_team PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../asset ${PROJECT_SOURCE_DIR}/src/editor/mcp/src)
target_compile_definitions(test_mcp_team PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp_team"
    MCE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
