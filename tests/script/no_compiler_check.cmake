# Fails when a binary in BINARIES (;-separated) contains the Luau compiler's entry point.
foreach(bin IN LISTS BINARIES)
    file(STRINGS "${bin}" hits REGEX "luau_compile|Luau::compile")
    if(hits)
        message(FATAL_ERROR "${bin} links the Luau compiler (the player loads bytecode only)")
    endif()
endforeach()
message(STATUS "no Luau compiler in: ${BINARIES}")
