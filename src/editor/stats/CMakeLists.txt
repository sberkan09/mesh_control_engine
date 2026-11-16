# mce_scene_stats (L6): `mce scene stats <scene>...` (M-SCENESTATS 2). Reads .mcscene files and prints what each one
# holds (mce/editor/scene_stats.h). Exit codes: 0 ok, 2 usage or a file that does not parse.
# The engine's own reflected components are known (the MCP server's set); a game module's are not.
mce_add_executable(mce_scene_stats UMBRELLA
    SOURCES
        main.cpp
        ../mcp/app/engine_components.cpp
    DEPS
        mce::mce_editor_core
        mce::mce_kit_novel
        mce::mce_module_host)
