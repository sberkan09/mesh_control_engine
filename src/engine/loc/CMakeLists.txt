# mce_loc (L2, M-MOBILE step 11a): string tables (mce/loc/string_table.h, the StringTable chunk the
# cook writes from assets/loc/<locale>.csv) and the Localization world resource the player installs
# (mce/loc/loc.h): language choice with a fallback chain, plural rules, placeholders, case mapping.
mce_add_library(mce_loc
    SOURCES
        src/loc.cpp
        src/string_table.cpp
    PUBLIC_DEPS
        mce::mce_core
    PRIVATE_DEPS
        mce::mce_ecs_scene)  # save::read / save::write for the player's language choice
