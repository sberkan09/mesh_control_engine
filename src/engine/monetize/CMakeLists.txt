# mce_monetize (L2, M-MONETIZE): in-app purchases and ads behind one engine API (mce/monetize/monetize.h),
# the store catalog chunk (mce/monetize/catalog.h) and the provider interfaces the platform stores and ad
# networks implement. Off unless a project's host turns it on (player::PlayerDesc::monetize).
mce_add_library(mce_monetize
    SOURCES
        src/catalog.cpp
        src/fake.cpp
        src/monetize.cpp
        src/platform_store.cpp
    PUBLIC_DEPS
        mce::mce_core
        mce::mce_ecs
    PRIVATE_DEPS
        mce::mce_ecs_scene
        mce::mce_platform)
