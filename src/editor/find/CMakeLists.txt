# mce_scene_find (L6): `mce scene find <scene>... [--name <part>] [--component <Type>] [--prefab]` (M-SCENEFIND 2). Reads
# .mcscene files and prints the entities that match (mce/editor/scene_find.h). Exit codes: 0 matches, 1 none, 2 usage or a
# file that does not parse. The engine's own reflected components are known (the MCP server's set); a game module's are not.
mce_add_executable(mce_scene_find UMBRELLA
    SOURCES
        main.cpp
        ../mcp/app/engine_components.cpp
    DEPS
        mce::mce_editor_core
        mce::mce_kit_novel
        mce::mce_module_host)
