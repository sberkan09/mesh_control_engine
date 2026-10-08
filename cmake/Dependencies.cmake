# Third-party dependencies, pinned by git tag (ADR-0017: FetchContent instead of vcpkg).
#
# Policy (ARCHITECTURE §7): mce_core links only spdlog + Tracy, ever. Every new dependency
# needs an ADR and an entry here. Cook-time-only deps must never reach mce_engine.

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# ---- spdlog (logging facade backend) ---------------------------------------
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_NO_EXCEPTIONS ON CACHE BOOL "" FORCE)
set(SPDLOG_ENABLE_PCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.17.0
    GIT_SHALLOW    TRUE
    SYSTEM)

# ---- Tracy (profiler client) -------------------------------------------------
set(TRACY_ENABLE ${MCE_ENABLE_TRACY} CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
set(TRACY_NO_BROADCAST ON CACHE BOOL "" FORCE)
FetchContent_Declare(tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG        v0.14.1
    GIT_SHALLOW    TRUE
    SYSTEM)

# ---- SDL3 (window, events, GL context, audio output; mce_platform + mce_rhi_gl only) ----
# Static, no subsystems we do not use yet. Gamepad joins with mce_input at v0.5.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.18
    GIT_SHALLOW    TRUE
    SYSTEM)

FetchContent_MakeAvailable(spdlog tracy SDL3)

# ---- glad2 (GL 4.5 core loader, generated, vendored in third_party/glad) ---------
# Regenerate: python -m glad --api gl:core=4.5 \
#     --extensions GL_ARB_shader_draw_parameters,GL_KHR_debug --out-path third_party/glad c
# Private to mce_rhi_gl (the GL wall): nothing else may link it.
add_library(mce_glad STATIC "${PROJECT_SOURCE_DIR}/third_party/glad/src/gl.c")
target_include_directories(mce_glad SYSTEM PUBLIC "${PROJECT_SOURCE_DIR}/third_party/glad/include")
set_target_properties(mce_glad PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- Dear ImGui, docking branch (editor only: mce_editor_layer is its sole consumer) ---------
# The ImGui wall (ARCHITECTURE §1.2 #3): nothing under src/engine/ may link or include it.
# Only the core library is compiled: the platform and renderer backends are our own
# (src/editor/layer), over mce::platform events and mce_rhi, so no GL or SDL_Event crosses a wall.
if(MCE_BUILD_EDITOR)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.92.9b-docking
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _no_cmake_)
    FetchContent_MakeAvailable(imgui)

    add_library(mce_imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_demo.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp")
    target_include_directories(mce_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}")
    target_compile_definitions(mce_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
    set_target_properties(mce_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(mce::imgui ALIAS mce_imgui)
endif()

# ---- doctest (tests only) ----------------------------------------------------
if(MCE_BUILD_TESTS)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.5.3
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _no_cmake_)   # header-only: we only need the include dir
    FetchContent_MakeAvailable(doctest)

    add_library(mce_doctest INTERFACE)
    target_include_directories(mce_doctest SYSTEM INTERFACE "${doctest_SOURCE_DIR}")
    # USE_STD_HEADERS: doctest's std::ostream forward declaration breaks MSVC 14.5x headers.
    target_compile_definitions(mce_doctest INTERFACE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
        DOCTEST_CONFIG_USE_STD_HEADERS)
    add_library(mce::doctest ALIAS mce_doctest)
endif()

# ---- DirectXMath (math test oracle only — core.md §2.7/§7, never linked into engine targets) ----
if(MCE_BUILD_TESTS)
    FetchContent_Declare(directxmath
        GIT_REPOSITORY https://github.com/microsoft/DirectXMath.git
        GIT_TAG        jun2026
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _no_cmake_)   # header-only: we only need Inc/
    FetchContent_MakeAvailable(directxmath)

    add_library(mce_directxmath INTERFACE)
    target_include_directories(mce_directxmath SYSTEM INTERFACE "${directxmath_SOURCE_DIR}/Inc")
    if(NOT WIN32)
        # DirectXMath needs a sal.h off Windows; a no-op annotation stub is enough.
        target_include_directories(mce_directxmath SYSTEM INTERFACE "${PROJECT_SOURCE_DIR}/tests/support/sal")
    endif()
    add_library(mce::directxmath ALIAS mce_directxmath)
endif()

# ---- stb (cook-time only: stb_image, stb_truetype, stb_rect_pack; stb_image_write in tests) ----
# Header-only, no release tags upstream: pinned by commit. Confined to mce_asset_cook (and tests)
# by the "stb wall" in scripts/ci/check_walls.py — never linked into a runtime target (FR-AST-010).
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        2c980bb59875b0d32144a71867fbdebb2f77cd20
    SOURCE_SUBDIR  _no_cmake_)
FetchContent_MakeAvailable(stb)

add_library(mce_stb_headers INTERFACE)
target_include_directories(mce_stb_headers SYSTEM INTERFACE "${stb_SOURCE_DIR}")
add_library(mce::stb_headers ALIAS mce_stb_headers)
