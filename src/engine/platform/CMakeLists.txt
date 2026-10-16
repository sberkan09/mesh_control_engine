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
        src/firebase.cpp
        src/game_services.cpp
        src/review.cpp
        src/haptics.cpp
        src/power.cpp
        src/screen_reader.cpp
        src/steam.cpp
    PUBLIC_DEPS
        mce::mce_core
    PRIVATE_DEPS
        SDL3::SDL3-static)

# The thermal state (platform/power.h) on iOS and macOS: Objective-C++ through the CXX language, as in
# mce_rhi_metal, so it gets the same compile policy.
if(APPLE)
    target_sources(mce_platform PRIVATE src/power_apple.mm)
    set_source_files_properties(src/power_apple.mm PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-xobjective-c++;-fno-objc-arc")
    target_link_libraries(mce_platform PRIVATE "-framework Foundation")
endif()

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

# The phone's review prompt (platform/review.h). Android: MceReview.java and Play In-App Review are
# stamped into every game's Gradle project. iOS: StoreKit's AppStore.requestReview (Swift), on by
# default since its Simulator run (M-MOBILE 11c); without it the iOS review backend is "none".
option(MCE_STORE_REVIEW "iOS: build StoreKit's review prompt (Swift) into mce_platform" ON)
if(IOS AND MCE_STORE_REVIEW)
    enable_language(Swift)
    target_sources(mce_platform PRIVATE src/review_storekit.swift)
    target_compile_definitions(mce_platform PRIVATE MCE_STORE_REVIEW=1)
    target_link_libraries(mce_platform PRIVATE "-framework StoreKit" "-framework UIKit")
endif()

# Haptics (platform/haptics.h). Android: MceHaptics.java is stamped into every game's Gradle project.
# iOS: UIKit's feedback generators (Swift), on by default since their Simulator run (M-MOBILE 11d);
# without them the iOS device haptics are "none" (gamepads still rumble).
option(MCE_UIKIT_HAPTICS "iOS: build UIKit's feedback generators (Swift) into mce_platform" ON)
if(IOS AND MCE_UIKIT_HAPTICS)
    enable_language(Swift)
    target_sources(mce_platform PRIVATE src/haptics_uikit.swift)
    target_compile_definitions(mce_platform PRIVATE MCE_UIKIT_HAPTICS=1)
    target_link_libraries(mce_platform PRIVATE "-framework UIKit")
endif()

# The screen reader (platform/screen_reader.h, M-ACCESS step 4). Android: MceActivity's mceAnnounce
# (TalkBack) is stamped into every game's Gradle project. The others stay opt-in until they have been
# run on their device: Windows (UI Automation notifications for Narrator, NVDA, JAWS), macOS
# (AppKit announcement requests) and iOS (UIAccessibility announcements, Swift); without them the
# backend is "none" and announcements reach only the sink.
option(MCE_SCREEN_READER_NATIVE "Windows, macOS and iOS: build the screen reader bridges into mce_platform" OFF)
if(MCE_SCREEN_READER_NATIVE)
    if(WIN32)
        target_sources(mce_platform PRIVATE src/screen_reader_win32.cpp)
        target_compile_definitions(mce_platform PRIVATE MCE_WIN32_ACCESS=1)
        target_link_libraries(mce_platform PRIVATE uiautomationcore oleaut32)
    elseif(IOS)
        enable_language(Swift)
        target_sources(mce_platform PRIVATE src/screen_reader_uikit.swift)
        target_compile_definitions(mce_platform PRIVATE MCE_UIKIT_ACCESS=1)
        target_link_libraries(mce_platform PRIVATE "-framework UIKit")
    elseif(APPLE)
        target_sources(mce_platform PRIVATE src/screen_reader_apple.mm)
        set_source_files_properties(src/screen_reader_apple.mm PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-xobjective-c++;-fno-objc-arc")
        target_compile_definitions(mce_platform PRIVATE MCE_APPKIT_ACCESS=1)
        target_link_libraries(mce_platform PRIVATE "-framework AppKit")
    endif()
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

# The phone's game services (platform/game_services.h). Android needs nothing here: MceGames.java and
# Play Games Services v2 are stamped by ship.py for a game with a .mcservices when
# MCE_PLAY_GAMES_APP_ID is set. iOS: GameKit (Swift, a system framework), opt-in until it has been run
# on a Mac; without it the iOS game services backend is "none" and everything stays on the device.
option(MCE_GAMEKIT "iOS: build Game Center (GameKit, Swift) into mce_platform" OFF)
if(IOS AND MCE_GAMEKIT)
    enable_language(Swift)
    target_sources(mce_platform PRIVATE src/game_services_gamekit.swift)
    target_compile_definitions(mce_platform PRIVATE MCE_GAMEKIT=1)
    target_link_libraries(mce_platform PRIVATE "-framework GameKit" "-framework UIKit")
endif()

# The live services vendor (platform/firebase.h): Crashlytics, Analytics and Remote Config. Android needs nothing
# here (MceFirebase.java and the Firebase Gradle plugins are stamped in by ship.py when
# MCE_FIREBASE_ANDROID_CONFIG names the project's google-services.json). iOS links Google's
# xcframeworks, which are not vendored: unzip Firebase's iOS distribution (Firebase.zip from the
# firebase-ios-sdk releases) and point MCE_FIREBASE_SDK_DIR at its folder holding FirebaseAnalytics/,
# FirebaseCrashlytics/ and FirebaseRemoteConfig/. Opt-in until it has been run on a Mac.
option(MCE_FIREBASE "iOS: build Firebase Crashlytics, Analytics and Remote Config into mce_platform" OFF)
set(MCE_FIREBASE_SDK_DIR "" CACHE PATH
    "iOS: the unzipped Firebase.zip (FirebaseAnalytics/, FirebaseCrashlytics/, FirebaseRemoteConfig/)")
if(IOS AND MCE_FIREBASE)
    if(NOT EXISTS "${MCE_FIREBASE_SDK_DIR}/FirebaseCrashlytics/FirebaseCrashlytics.xcframework" OR
       NOT EXISTS "${MCE_FIREBASE_SDK_DIR}/FirebaseRemoteConfig/FirebaseRemoteConfig.xcframework")
        message(FATAL_ERROR "MCE_FIREBASE needs MCE_FIREBASE_SDK_DIR with FirebaseAnalytics/, FirebaseCrashlytics/ and FirebaseRemoteConfig/ (got '${MCE_FIREBASE_SDK_DIR}')")
    endif()
    enable_language(Swift)
    if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
        set(_mce_slice "ios-arm64_x86_64-simulator")
    else()
        set(_mce_slice "ios-arm64")
    endif()
    file(GLOB _mce_firebase_fws "${MCE_FIREBASE_SDK_DIR}/FirebaseAnalytics/*.xcframework"
         "${MCE_FIREBASE_SDK_DIR}/FirebaseCrashlytics/*.xcframework"
         "${MCE_FIREBASE_SDK_DIR}/FirebaseRemoteConfig/*.xcframework")
    list(REMOVE_DUPLICATES _mce_firebase_fws)
    target_sources(mce_platform PRIVATE src/firebase_ios.swift)
    target_compile_definitions(mce_platform PRIVATE MCE_FIREBASE=1)
    set(_mce_firebase_seen)
    foreach(fw ${_mce_firebase_fws})
        get_filename_component(name "${fw}" NAME_WE)
        if(name IN_LIST _mce_firebase_seen)
            continue()  # both folders ship the shared ones (FirebaseCore, GoogleUtilities, ...)
        endif()
        list(APPEND _mce_firebase_seen ${name})
        target_compile_options(mce_platform PRIVATE "$<$<COMPILE_LANGUAGE:Swift>:-F${fw}/${_mce_slice}>")
        target_link_options(mce_platform INTERFACE "-F${fw}/${_mce_slice}")
        target_link_libraries(mce_platform PRIVATE "-framework ${name}")
    endforeach()
    # Firebase's static frameworks register Objective-C categories: -ObjC keeps them.
    target_link_options(mce_platform INTERFACE "-ObjC")
    target_link_libraries(mce_platform PRIVATE sqlite3 z "-framework StoreKit" "-framework SystemConfiguration"
        "-framework UIKit")
endif()

if(MCE_WEB)
    add_subdirectory(web_probe)
endif()
