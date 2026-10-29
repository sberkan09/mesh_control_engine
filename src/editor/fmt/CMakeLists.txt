# mce_scene_fmt (L6): `mce scene fmt <scene>... [--write]` (M-SCENEFMT). Checks that .mcscene files are in the engine's
# canonical text (what SceneDocument::serialize writes), or rewrites them. Exit codes: 0 canonical (or written), 1 some
# are not, 2 usage or a file that does not parse. The engine's own reflected components are known (the MCP server's set);
# a game module's are not.
mce_add_executable(mce_scene_fmt UMBRELLA
    SOURCES
        main.cpp
        ../mcp/app/engine_components.cpp
    DEPS
        mce::mce_editor_core
        mce::mce_kit_novel
        mce::mce_module_host)
