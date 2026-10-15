# Web builds (M-WEB, ADR-0039): Emscripten (the web-* presets, EMSDK in the environment). Included
# by the root CMakeLists.txt before any target is defined. Sets MCE_WEB and the link options every
# engine executable needs in a browser.
#
#   - graphics are WebGPU through mce_rhi_webgpu and Dawn's emdawnwebgpu bindings;
#   - the app loop stays a plain loop (app::run): ASYNCIFY lets it yield to the browser once a frame
#     (core/src/app.cpp) and lets the device wait for WebGPU's adapter, device and buffer maps;
#   - an executable links to <name>.html + .js + .wasm; the default shell page holds the canvas.
#
# A cross build cannot run the target's mce_cook, so packs come from a host build (MCE_HOST_COOK),
# exactly as for phones (cmake/MceMobile.cmake).

if(EMSCRIPTEN)
    set(MCE_WEB ON)
else()
    set(MCE_WEB OFF)
endif()

if(MCE_WEB)
    set(CMAKE_EXECUTABLE_SUFFIX ".html")
    set(MCE_WEB_LINK_OPTIONS
        -sASYNCIFY=1
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=64MB
        -sSTACK_SIZE=1MB
        -sEXIT_RUNTIME=1
        -sENVIRONMENT=web)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND MCE_WEB_LINK_OPTIONS -sASSERTIONS=2)
    endif()
    # Linking runs wasm-opt's ASYNCIFY pass, about 6 GB per executable: one link at a time (Ninja).
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS mce_web_link=1)
    set(CMAKE_JOB_POOL_LINK mce_web_link)

    # Every engine executable gets the web link options (wrapping add_executable like MceMobile).
    function(add_executable name)
        _add_executable(${ARGV})
        if(ARGC GREATER 1 AND (ARGV1 STREQUAL "IMPORTED" OR ARGV1 STREQUAL "ALIAS"))
            return()
        endif()
        cmake_path(IS_PREFIX PROJECT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE ours)
        if(ours AND NOT CMAKE_CURRENT_SOURCE_DIR MATCHES "/third_party/|/_deps/|/_fetch/")
            target_link_options(${name} PRIVATE ${MCE_WEB_LINK_OPTIONS})
        endif()
    endfunction()
endif()
