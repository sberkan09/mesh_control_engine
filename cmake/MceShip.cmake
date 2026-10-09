# mce_ship_metadata(<target> NAME <display name> DESCRIPTION <text> ICON <file.ico>)
#
# Shipping metadata for a game executable (ROADMAP M4 "one-command ship v1: icon/version"). On
# Windows it compiles a VERSIONINFO + icon resource from the project version; elsewhere the icon is
# set at run time from the cooked pack (platform::Window::set_icon) and the version travels in the
# zip's VERSION.txt (scripts/ship.py). The resource script is generated, never hand-edited.
function(mce_ship_metadata target)
    cmake_parse_arguments(ARG "" "NAME;DESCRIPTION;ICON" "" ${ARGN})
    if(NOT WIN32)
        return()
    endif()
    set(MCE_RC_NAME "${ARG_NAME}")
    set(MCE_RC_DESCRIPTION "${ARG_DESCRIPTION}")
    set(MCE_RC_ICON "${ARG_ICON}")
    set(MCE_RC_EXE "${target}.exe")
    set(MCE_RC_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
    set(MCE_RC_VERSION_MINOR ${PROJECT_VERSION_MINOR})
    set(MCE_RC_VERSION_PATCH ${PROJECT_VERSION_PATCH})
    set(MCE_RC_VERSION "${PROJECT_VERSION}")
    set(rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_ship.rc")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ship.rc.in" "${rc}" @ONLY)  # also installed with the SDK
    target_sources(${target} PRIVATE "${rc}")
endfunction()
