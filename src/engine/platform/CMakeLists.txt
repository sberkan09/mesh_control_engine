# mce_platform (L1): SDL3 window + event pump, gamepads, wall clock, boot-failure message box.
# The SDL wall (ARCHITECTURE §1.2 #2): SDL headers and SDL_Event live only here. SDL3 is a PRIVATE
# dependency; the one sanctioned leak is Window::native_handle() (an SDL_Window*, typed void*).

mce_add_library(mce_platform
    SOURCES
        src/platform.cpp
        src/window.cpp
        src/audio_output.cpp
        src/library.cpp
        src/gamepad.cpp
        src/process.cpp
        src/store.cpp
        src/ads.cpp
    PUBLIC_DEPS
        mce::mce_core
    PRIVATE_DEPS
        SDL3::SDL3-static)

# The phone's purchase store (platform/store.h). Android needs nothing here: the Play Billing side is
# Java, stamped into the game's Gradle project. iOS StoreKit 2 is Swift and stays opt-in until it has
# been verified on a Mac (Simulator StoreKit configuration); without it the iOS store is "none".
option(MCE_STOREKIT "iOS: build the StoreKit 2 store (Swift) into mce_platform" OFF)
if(IOS AND MCE_STOREKIT)
    enable_language(Swift)
    target_sources(mce_platform PRIVATE src/store_storekit.swift)
    target_compile_definitions(mce_platform PRIVATE MCE_STOREKIT=1)
    target_link_libraries(mce_platform PRIVATE "-framework StoreKit")
endif()

# The phone's ad network (platform/ads.h). Android again needs nothing here (MceAds.java is stamped
# into the Gradle project). iOS links Google's xcframeworks, which are not vendored: download the
# Google Mobile Ads SDK (11 or newer, with UserMessagingPlatform) and point MCE_ADMOB_SDK_DIR at the
# folder holding GoogleMobileAds.xcframework and UserMessagingPlatform.xcframework. Opt-in until it
# has been run on a Mac.
option(MCE_ADMOB "iOS: build Google Mobile Ads, UMP and ATT into mce_platform" OFF)
set(MCE_ADMOB_SDK_DIR "" CACHE PATH "iOS: folder with GoogleMobileAds.xcframework and UserMessagingPlatform.xcframework")
if(IOS AND MCE_ADMOB)
    if(NOT EXISTS "${MCE_ADMOB_SDK_DIR}/GoogleMobileAds.xcframework")
        message(FATAL_ERROR "MCE_ADMOB needs MCE_ADMOB_SDK_DIR with GoogleMobileAds.xcframework (got '${MCE_ADMOB_SDK_DIR}')")
    endif()
    enable_language(Swift)
    if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
        set(_mce_slice "ios-arm64_x86_64-simulator")
    else()
        set(_mce_slice "ios-arm64")
    endif()
    set(_mce_fw_dirs)
    foreach(fw GoogleMobileAds UserMessagingPlatform)
        list(APPEND _mce_fw_dirs "${MCE_ADMOB_SDK_DIR}/${fw}.xcframework/${_mce_slice}")
    endforeach()
    target_sources(mce_platform PRIVATE src/ads_admob.swift)
    target_compile_definitions(mce_platform PRIVATE MCE_ADMOB=1)
    foreach(d ${_mce_fw_dirs})
        target_compile_options(mce_platform PRIVATE "$<$<COMPILE_LANGUAGE:Swift>:-F${d}>")
        target_link_options(mce_platform INTERFACE "-F${d}")
    endforeach()
    target_link_libraries(mce_platform PRIVATE "-framework GoogleMobileAds" "-framework UserMessagingPlatform"
        "-framework AppTrackingTransparency" "-framework UIKit")
endif()
