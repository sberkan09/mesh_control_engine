# mce_scene_diff (L6): `mce scene diff <a> <b>` (M-SCENEDIFF 2). Reads two .mcscene files and prints what changed
# between them in the document's own terms (mce/editor/scene_diff.h). Exit codes: 0 identical, 1 they differ,
# 2 usage or a file that does not parse. The engine's own reflected components are known (the MCP server's set);
# a game module's are not, so a scene that uses one is refused with the parser's diagnostic.
mce_add_executable(mce_scene_diff UMBRELLA
    SOURCES
        main.cpp
        ../mcp/app/engine_components.cpp
    DEPS
        mce::mce_editor_core
        mce::mce_kit_novel
        mce::mce_module_host)
