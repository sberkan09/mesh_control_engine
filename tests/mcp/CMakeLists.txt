# Editor MCP server (editor-mcp.md §6): in-process protocol/tool tests + the FR-EDT-160 gate + fuzz,
# a transcript test that drives the real mce_mcp binary over pipes against golden responses, and
# a symbol wall (no ImGui, no socket API in mce_mcp*).
mce_add_test(test_mcp
    SOURCES
        test_mcp.cpp
    DEPS
        mce::mce_mcp_core
        mce::mce_platform)
target_include_directories(test_mcp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../asset)
target_compile_definitions(test_mcp PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp"
    MCE_SOURCE_DIR="${PROJECT_SOURCE_DIR}" MCE_PLAYER_EXE="$<TARGET_FILE:mce_player>")
add_dependencies(test_mcp mce_player)

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
