# mce_liveops (L2, M-LIVEOPS): services for shipped games, off unless a project's host turns them on
# (player::PlayerDesc::liveops): the consent gate (mce/liveops/consent.h), analytics events
# (mce/liveops/analytics.h) and crash reports sent on the next launch (mce/liveops/crash_upload.h).
# Never in the sim or the determinism hash.
mce_add_library(mce_liveops
    SOURCES
        src/analytics.cpp
        src/crash_upload.cpp
        src/liveops.cpp
    PUBLIC_DEPS
        mce::mce_core)
