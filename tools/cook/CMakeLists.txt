# mce_cook (L6): CLI over the cook library. Exit codes: 0 ok, 1 cook errors, 2 usage (FR-AST-070).
mce_add_executable(mce_cook
    SOURCES
        main.cpp
    DEPS
        mce::mce_asset_cook)
