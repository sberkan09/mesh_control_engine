# mce_liveops (L2, M-LIVEOPS): services for shipped games, off unless a project's host turns them on
# (player::PlayerDesc::liveops): the consent gate (mce/liveops/consent.h), analytics events
# (mce/liveops/analytics.h), crash reports sent on the next launch (mce/liveops/crash_upload.h),
# game services (mce/liveops/game_services.h) and remote config (mce/liveops/remote_config.h).
# Never in the sim or the determinism hash.
mce_add_library(mce_liveops
    SOURCES
        src/analytics.cpp
        src/crash_upload.cpp
        src/game_services.cpp
        src/game_services_catalog.cpp
        src/local_game_services.cpp
        src/liveops.cpp
        src/remote_config.cpp
    PUBLIC_DEPS
        mce::mce_core
        mce::mce_ecs
    PRIVATE_DEPS
        mce::mce_asset       # FileConfigFetcher reads a cooked pack
        mce::mce_ecs_scene)  # save::read / save::write for the fetched values and game services state
