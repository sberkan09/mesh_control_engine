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
