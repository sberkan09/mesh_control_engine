# mce_scene_lint (L6): `mce scene lint <scene>...` (M-SCENELINT 2). Reads .mcscene files and prints the mistakes in
# each one (mce/editor/scene_lint.h). Exit codes: 0 no findings, 1 findings, 2 usage or a file that does not parse.
# The engine's own reflected components are known (the MCP server's set); a game module's are not.
mce_add_executable(mce_scene_lint UMBRELLA
    SOURCES
        main.cpp
        ../mcp/app/engine_components.cpp
    DEPS
        mce::mce_editor_core
        mce::mce_kit_novel
        mce::mce_module_host)
