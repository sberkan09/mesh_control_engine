# mce_cook (L6): CLI over the cook library. Exit codes: 0 ok, 1 cook errors, 2 usage (FR-AST-070).
# The engine component libraries are linked for their reflected types only (engine_components.cpp);
# --module <game module> adds a game's (mce_module_host, the umbrella where modules are supported).
mce_add_executable(mce_cook UMBRELLA
    SOURCES
        main.cpp
        engine_components.cpp
    DEPS
        mce::mce_asset_cook
        mce::mce_gameplay_kit
        mce::mce_kit_novel
        mce::mce_physics2d
        mce::mce_audio
        mce::mce_ecs_scene
        mce::mce_scene
        mce::mce_module_host)
