# Editor MCP server (editor-mcp.md, FR-EDT-160, ADR-0027): two targets.
#   mce_mcp_core  (L5) JSON-RPC protocol, tool table, value codec, headless run. Headless and
#                 ImGui-free: links mce_editor_core, never mce_editor_layer, never a socket.
#   mce_mcp       (L6) the executable: the MCP stdio transport over mce_mcp_core.
# The JSON codec is the cook's (mce/cook/json.h); no runtime target links either target
# (scripts/ci/check_walls.py link wall).

mce_add_library(mce_mcp_core UMBRELLA
    SOURCES
        src/agent_gen.cpp
        src/agent_image.cpp
        src/agent_sim.cpp
        src/asset_tools.cpp
        src/headless_run.cpp
        src/json_writer.cpp
        src/prefab_tools.cpp
        src/project_tools.cpp
        src/server.cpp
        src/tools.cpp
        src/tools_agent.cpp
        src/tools_check.cpp
        src/tools_team.cpp
        src/tools_ui.cpp
        src/tools_timeline.cpp
        src/tools_friction.cpp
        src/tools_code.cpp
        src/tools_content.cpp
        src/tools_docs.cpp
        src/tools_render.cpp
        src/tools_test.cpp
        src/values.cpp
    PUBLIC_DEPS
        mce::mce_editor_core
        mce::mce_input)
# mce://docs/... in a build tree: the sources it was configured from (an installed SDK serves its
# share/doc/mce instead, found beside the server first).
target_compile_definitions(mce_mcp_core PRIVATE MCE_MCP_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")

mce_add_executable(mce_mcp UMBRELLA
    SOURCES
        app/main.cpp
        app/engine_components.cpp
    DEPS
        mce::mce_mcp_core
        mce::mce_kit_novel  # its components, so a novel project's scenes open without --module
        mce::mce_module_host)
