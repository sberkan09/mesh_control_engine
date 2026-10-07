# mce_platform (L1): SDL3 window + event pump, wall clock, boot-failure message box.
# The SDL wall (ARCHITECTURE §1.2 #2): SDL headers and SDL_Event live only here. SDL3 is a PRIVATE
# dependency; the one sanctioned leak is Window::native_handle() (an SDL_Window*, typed void*).

mce_add_library(mce_platform
    SOURCES
        src/platform.cpp
        src/window.cpp
    PUBLIC_DEPS
        mce::mce_core
    PRIVATE_DEPS
        SDL3::SDL3-static)
