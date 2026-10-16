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
# EXCLUDE_FROM_ALL: Tracy's own install rules stay out of the SDK (cmake/MceInstall.cmake); the
# client library is still built as mce_core's dependency.
FetchContent_Declare(tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG        v0.14.1
    GIT_SHALLOW    TRUE
    SYSTEM
    EXCLUDE_FROM_ALL)

# ---- SDL3 (window, events, GL context, audio output; mce_platform + mce_rhi_gl only) ----
# Static, no subsystems we do not use yet.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
# SDL GPU + Vulkan only for mce_rhi_sdlgpu (ADR-0038); SDL loads the Vulkan loader at run time.
set(SDL_GPU ${MCE_RHI_SDLGPU} CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN ${MCE_RHI_SDLGPU} CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
# M6 input (ADR-0026): gamepads for mce_input — joystick/gamepad API, HIDAPI drivers, rumble, and
# the virtual-joystick driver the input tests and bots drive.
set(SDL_JOYSTICK ON CACHE BOOL "" FORCE)
set(SDL_HIDAPI ON CACHE BOOL "" FORCE)
set(SDL_HAPTIC ON CACHE BOOL "" FORCE)
set(SDL_VIRTUAL_JOYSTICK ON CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.18
    GIT_SHALLOW    TRUE
    # iOS Simulator support for SDL GPU's Metal driver; a no-op outside the Simulator (M-MOBILE).
    PATCH_COMMAND  ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_LIST_DIR}/mobile/ios/patch_sdl_simulator.cmake"
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

# ---- Box2D v3 (2D physics test oracle only — ADR-0016, never linked into engine targets) ----
# Linked into test_physics2d alone (the Box2D wall in scripts/ci/check_walls.py). Static, no
# samples/tests of its own; SIMD stays on its scalar-compatible SSE2 default (no AVX2).
if(MCE_BUILD_TESTS AND MCE_PHYSICS_ORACLE)
    set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
    set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(BOX2D_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(BOX2D_DOCS OFF CACHE BOOL "" FORCE)
    set(BOX2D_PROFILE OFF CACHE BOOL "" FORCE)
    set(BOX2D_AVX2 OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(box2d
        GIT_REPOSITORY https://github.com/erincatto/box2d.git
        GIT_TAG        v3.1.1
        GIT_SHALLOW    TRUE
        SYSTEM
        EXCLUDE_FROM_ALL)  # a test oracle: never installed with the SDK
    FetchContent_MakeAvailable(box2d)
endif()

# ---- stb (cook-time: stb_image, stb_truetype, stb_rect_pack; stb_image_write in tests) ----------
# Header-only, no release tags upstream: pinned by commit. Confined to mce_asset_cook (and tests)
# by the "stb wall" in scripts/ci/check_walls.py — never linked into a runtime target (FR-AST-010).
# The one exception is stb_vorbis, the Ogg Vorbis decoder compiled into mce_asset (ADR-0044, the
# "stb_vorbis wall"): it reads from memory, allocates only from the reader's arena, and is fetched on
# every build, cross builds included.
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        2c980bb59875b0d32144a71867fbdebb2f77cd20
    SOURCE_SUBDIR  _no_cmake_)
FetchContent_MakeAvailable(stb)

add_library(mce_stb_headers INTERFACE)
target_include_directories(mce_stb_headers SYSTEM INTERFACE "${stb_SOURCE_DIR}")
add_library(mce::stb_headers ALIAS mce_stb_headers)

# ---- meshoptimizer (cook-time only: vertex cache / fetch optimization of glTF meshes) ----
# Confined to mce_asset_cook by the "meshopt wall" in scripts/ci/check_walls.py (FR-AST-010):
# runtimes read the already-optimized CHNK_MESH. Only the sources the cook calls are compiled.
FetchContent_Declare(meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG        v0.25
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _no_cmake_)
FetchContent_MakeAvailable(meshoptimizer)

add_library(mce_meshopt STATIC
    "${meshoptimizer_SOURCE_DIR}/src/indexgenerator.cpp"
    "${meshoptimizer_SOURCE_DIR}/src/simplifier.cpp"
    "${meshoptimizer_SOURCE_DIR}/src/vcacheoptimizer.cpp"
    "${meshoptimizer_SOURCE_DIR}/src/vfetchoptimizer.cpp")
target_include_directories(mce_meshopt SYSTEM PUBLIC "${meshoptimizer_SOURCE_DIR}/src")
set_target_properties(mce_meshopt PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(mce_meshopt PRIVATE /W0)
else()
    target_compile_options(mce_meshopt PRIVATE -w)
endif()

# ---- astc-encoder (cook-time only: ASTC 4x4 textures for phones, mce_cook --textures astc) ----
# Confined to the cook's texture importer by the "astcenc wall" in scripts/ci/check_walls.py. Built
# from its sources (its own CMake project pulls in CTest and LTO): the portable no-SIMD path with
# floating-point invariance, so a pack cooks to the same bytes on every host.
if(NOT CMAKE_CROSSCOMPILING)
    FetchContent_Declare(astcenc
        GIT_REPOSITORY https://github.com/ARM-software/astc-encoder.git
        GIT_TAG        5.6.0
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _no_cmake_)
    FetchContent_MakeAvailable(astcenc)
    set(_astc "${astcenc_SOURCE_DIR}/Source")
    add_library(mce_astcenc STATIC
        ${_astc}/astcenc_averages_and_directions.cpp ${_astc}/astcenc_block_sizes.cpp
        ${_astc}/astcenc_color_quantize.cpp ${_astc}/astcenc_color_unquantize.cpp
        ${_astc}/astcenc_compress_symbolic.cpp ${_astc}/astcenc_compute_variance.cpp
        ${_astc}/astcenc_decompress_symbolic.cpp ${_astc}/astcenc_diagnostic_trace.cpp ${_astc}/astcenc_entry.cpp
        ${_astc}/astcenc_find_best_partitioning.cpp ${_astc}/astcenc_ideal_endpoints_and_weights.cpp
        ${_astc}/astcenc_image.cpp ${_astc}/astcenc_integer_sequence.cpp ${_astc}/astcenc_mathlib.cpp
        ${_astc}/astcenc_mathlib_softfloat.cpp ${_astc}/astcenc_partition_tables.cpp
        ${_astc}/astcenc_percentile_tables.cpp ${_astc}/astcenc_pick_best_endpoint_format.cpp
        ${_astc}/astcenc_quantization.cpp ${_astc}/astcenc_symbolic_physical.cpp ${_astc}/astcenc_weight_align.cpp
        ${_astc}/astcenc_weight_quant_xfer_tables.cpp)
    target_include_directories(mce_astcenc SYSTEM PUBLIC "${_astc}")
    target_compile_definitions(mce_astcenc PRIVATE ASTCENC_NEON=0 ASTCENC_SVE=0 ASTCENC_SSE=0 ASTCENC_AVX=0
                                                   ASTCENC_POPCNT=0 ASTCENC_F16C=0)
    set_target_properties(mce_astcenc PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(mce_astcenc PRIVATE /W0 /fp:precise)
    else()
        target_compile_options(mce_astcenc PRIVATE -w -ffp-contract=off)
    endif()
endif()

# ---- libogg + libvorbis (cook-time only: Ogg Vorbis encoding of audio clips — ADR-0044) ----------
# The encoder half of the Xiph reference codec, built from its sources like astcenc (its own CMake
# needs find_package(Ogg) and installs). Confined to the cook's audio importer by the "libvorbis wall"
# in scripts/ci/check_walls.py; the runtime decodes with stb_vorbis (mce_asset). Host builds only:
# a cross build cooks with the host's mce_cook. -ffp-contract=off keeps one host's output repeatable.
if(NOT CMAKE_CROSSCOMPILING)
    FetchContent_Declare(ogg
        GIT_REPOSITORY https://github.com/xiph/ogg.git
        GIT_TAG        v1.3.5
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _no_cmake_)
    FetchContent_Declare(vorbis
        GIT_REPOSITORY https://github.com/xiph/vorbis.git
        GIT_TAG        v1.3.7
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _no_cmake_)
    FetchContent_MakeAvailable(ogg vorbis)
    set(_ogg_gen "${CMAKE_BINARY_DIR}/_deps/ogg-config/ogg")
    file(MAKE_DIRECTORY "${_ogg_gen}")
    file(WRITE "${_ogg_gen}/config_types.h.tmp" "#ifndef __CONFIG_TYPES_H__\n#define __CONFIG_TYPES_H__\n#include <stdint.h>\n"
        "typedef int16_t ogg_int16_t;\ntypedef uint16_t ogg_uint16_t;\ntypedef int32_t ogg_int32_t;\n"
        "typedef uint32_t ogg_uint32_t;\ntypedef int64_t ogg_int64_t;\ntypedef uint64_t ogg_uint64_t;\n#endif\n")
    configure_file("${_ogg_gen}/config_types.h.tmp" "${_ogg_gen}/config_types.h" COPYONLY)
    set(_vl "${vorbis_SOURCE_DIR}/lib")
    add_library(mce_vorbisenc STATIC
        "${ogg_SOURCE_DIR}/src/bitwise.c" "${ogg_SOURCE_DIR}/src/framing.c"
        ${_vl}/mdct.c ${_vl}/smallft.c ${_vl}/block.c ${_vl}/envelope.c ${_vl}/window.c ${_vl}/lsp.c ${_vl}/lpc.c
        ${_vl}/analysis.c ${_vl}/synthesis.c ${_vl}/psy.c ${_vl}/info.c ${_vl}/floor1.c ${_vl}/floor0.c ${_vl}/res0.c
        ${_vl}/mapping0.c ${_vl}/registry.c ${_vl}/codebook.c ${_vl}/sharedbook.c ${_vl}/lookup.c ${_vl}/bitrate.c
        ${_vl}/vorbisenc.c)
    target_include_directories(mce_vorbisenc SYSTEM PUBLIC "${ogg_SOURCE_DIR}/include" "${CMAKE_BINARY_DIR}/_deps/ogg-config"
                               "${vorbis_SOURCE_DIR}/include")
    target_include_directories(mce_vorbisenc PRIVATE "${_vl}")
    set_target_properties(mce_vorbisenc PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(mce_vorbisenc PRIVATE /W0 /fp:precise)
    else()
        target_compile_options(mce_vorbisenc PRIVATE -w -ffp-contract=off)
        target_link_libraries(mce_vorbisenc PRIVATE m)
    endif()
    add_library(mce::vorbisenc ALIAS mce_vorbisenc)
endif()

# ---- Jolt Physics 5 (3D physics; mce_physics3d only — ADR-0003) ----------------------------------
# The JPH wall in scripts/ci/check_walls.py confines Jolt includes and JPH:: symbols to
# src/engine/physics3d/. Built through Jolt's own Build/CMakeLists.txt, which keeps its flag
# changes in its own directory scope; everything that would fight our compile policy is off:
# no exceptions/RTTI, no LTO, no CXX flag override, no static MSVC runtime, no GPU compute,
# no debug renderer/profiler/object streams. CROSS_PLATFORM_DETERMINISTIC gives precise FP
# without FMA contraction (ADR-0007), and the SIMD level stops at SSE4.2 so the binary runs on
# the same machines as the rest of the engine (ADR-0018).
set(JOLT_OPTIONS
    CPP_EXCEPTIONS_ENABLED=OFF CPP_RTTI_ENABLED=OFF OVERRIDE_CXX_FLAGS=OFF INTERPROCEDURAL_OPTIMIZATION=OFF
    ENABLE_ALL_WARNINGS=OFF ENABLE_INSTALL=OFF GENERATE_DEBUG_SYMBOLS=OFF USE_STATIC_MSVC_RUNTIME_LIBRARY=OFF
    FLOATING_POINT_EXCEPTIONS_ENABLED=OFF CROSS_PLATFORM_DETERMINISTIC=ON DOUBLE_PRECISION=OFF USE_ASSERTS=OFF
    USE_AVX=OFF USE_AVX2=OFF USE_AVX512=OFF USE_LZCNT=OFF USE_TZCNT=OFF USE_F16C=OFF USE_FMADD=OFF
    DEBUG_RENDERER_IN_DEBUG_AND_RELEASE=OFF PROFILER_IN_DEBUG_AND_RELEASE=OFF ENABLE_OBJECT_STREAM=OFF
    JPH_USE_DX12=OFF JPH_USE_VK=OFF JPH_USE_MTL=OFF JPH_USE_CPU_COMPUTE=OFF JPH_BUILD_SHARED_LIBS=OFF)
foreach(opt IN LISTS JOLT_OPTIONS)
    string(REPLACE "=" ";" kv "${opt}")
    list(GET kv 0 k)
    list(GET kv 1 v)
    set(${k} ${v} CACHE BOOL "" FORCE)
endforeach()
set(OBJECT_LAYER_BITS 32 CACHE STRING "" FORCE)  # 16-bit MCE layer group + 16-bit mask (ADR-0003 §4)
FetchContent_Declare(JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG        v5.6.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  Build
    SYSTEM)
FetchContent_MakeAvailable(JoltPhysics)
set_target_properties(Jolt PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(Jolt PRIVATE /W0)
else()
    target_compile_options(Jolt PRIVATE -w)
endif()

# ---- Recast & Detour (3D navigation; mce_physics3d only — ADR-0040) ------------------------------
# Navmesh bake (Recast), path queries and crowds (Detour, DetourCrowd) and carved obstacles
# (DetourTileCache). Zlib licensed. Compiled straight into mce_physics3d (an object library folded
# into its archive, like ENet into mce_net) with our flags: warnings off, and no FP contraction so a
# bake is byte-identical run to run on one platform (ADR-0007). The Recast wall in
# scripts/ci/check_walls.py confines its headers and symbols to src/engine/physics3d/src/.
FetchContent_Declare(recastnavigation
    GIT_REPOSITORY https://github.com/recastnavigation/recastnavigation.git
    GIT_TAG        v1.6.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  _no_cmake_)  # its sources are compiled below with our flags
FetchContent_MakeAvailable(recastnavigation)
set(_mce_recast_dirs Recast Detour DetourCrowd DetourTileCache)
set(_mce_recast_sources "")
set(_mce_recast_includes "")
foreach(dir IN LISTS _mce_recast_dirs)
    file(GLOB _srcs CONFIGURE_DEPENDS "${recastnavigation_SOURCE_DIR}/${dir}/Source/*.cpp")
    list(APPEND _mce_recast_sources ${_srcs})
    list(APPEND _mce_recast_includes "${recastnavigation_SOURCE_DIR}/${dir}/Include")
endforeach()
add_library(mce_recast OBJECT ${_mce_recast_sources})
target_include_directories(mce_recast SYSTEM PUBLIC ${_mce_recast_includes})
set_target_properties(mce_recast PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(mce_recast PRIVATE /W0 /fp:precise)
else()
    target_compile_options(mce_recast PRIVATE -w -ffp-contract=off)
endif()

# ---- glslang (shader cook, every platform) ---------------------------------------------------
# The cook validates .glsl stages and compiles their variants to SPIR-V (ADR-0033). Same tag and
# options as cmake/macos/MetalDependencies.cmake: this declaration comes first, so on macOS the
# Metal backend links this same glslang and only adds SPIRV-Cross. Linked privately by
# mce_asset_cook (a cook-time library, never a runtime target).
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG        16.6.0
    GIT_SHALLOW    TRUE
    SYSTEM)
FetchContent_MakeAvailable(glslang)
foreach(t glslang glslang-default-resource-limits)
    if(TARGET ${t})
        set_target_properties(${t} PROPERTIES POSITION_INDEPENDENT_CODE ON)
        if(MSVC)
            target_compile_options(${t} PRIVATE /W0)
        else()
            target_compile_options(${t} PRIVATE -w)
        endif()
    endif()
endforeach()

# ---- Tint (web builds only: GLSL -> SPIR-V -> WGSL in mce_rhi_webgpu, ADR-0039) ------------
if(MCE_WEB)
    include("${CMAKE_CURRENT_LIST_DIR}/web/TintDependencies.cmake")
endif()

# ---- pl_mpeg (video: MPEG-1 video + MP2 audio; mce_render only — ADR-0043) -------------------
# One MIT-licensed C header, no release tags upstream: pinned by commit. Its implementation is
# compiled into mce_render (src/plmpeg_impl.c, warnings off), which wraps it as render::VideoDecoder
# for the player's VideoPlayer textures and the cook's .mpg importer. Confined to
# src/engine/render/src/ by the pl_mpeg wall in scripts/ci/check_walls.py.
FetchContent_Declare(pl_mpeg
    GIT_REPOSITORY https://github.com/phoboslab/pl_mpeg.git
    GIT_TAG        c871f2be022ece7ef4f64230b4fb8e1fb9eb6023
    SOURCE_SUBDIR  _no_cmake_)
FetchContent_MakeAvailable(pl_mpeg)

# ---- ENet (optional networking; mce_net only — ADR-0035) ------------------------------------
# Reliable/unreliable sequenced channels over UDP. Compiled straight into mce_net (an object
# library folded into its archive), so the opt-in module is one static library in the tree and in
# the SDK. Confined to src/engine/net/src/ by the ENet wall in scripts/ci/check_walls.py; never part
# of mce_engine, so games that do not link mce_net carry no socket code.
if(MCE_WITH_NET)
    FetchContent_Declare(enet
        GIT_REPOSITORY https://github.com/lsalzman/enet.git
        GIT_TAG        v1.3.18
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  _no_cmake_)  # its sources are compiled below with our flags
    FetchContent_MakeAvailable(enet)

    add_library(mce_enet OBJECT
        "${enet_SOURCE_DIR}/callbacks.c" "${enet_SOURCE_DIR}/compress.c" "${enet_SOURCE_DIR}/host.c"
        "${enet_SOURCE_DIR}/list.c" "${enet_SOURCE_DIR}/packet.c" "${enet_SOURCE_DIR}/peer.c"
        "${enet_SOURCE_DIR}/protocol.c" "${enet_SOURCE_DIR}/unix.c" "${enet_SOURCE_DIR}/win32.c")
    target_include_directories(mce_enet SYSTEM PUBLIC "${enet_SOURCE_DIR}/include")
    set_target_properties(mce_enet PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(mce_enet PRIVATE /W0)
        target_compile_definitions(mce_enet PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(mce_enet PRIVATE -w)
        # The configure checks ENet's own CMake would run; every platform we build has these.
        target_compile_definitions(mce_enet PRIVATE HAS_FCNTL=1 HAS_POLL=1 HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1
            HAS_INET_PTON=1 HAS_INET_NTOP=1 HAS_MSGHDR_FLAGS=1 HAS_SOCKLEN_T=1)
    endif()
endif()

# ---- Luau (scripting spike; mce_script only — ADR-0041) ------------------------------------
# A sandboxed Luau VM for the M-SCRIPT spike. Only the VM, the compiler and their parts are built
# (EXCLUDE_FROM_ALL keeps the CLI, analysis and codegen targets out of `all`). Determinism
# (FR-RUN-040, ADR-0007): the VM and the compiler, whose constant folding runs on doubles, build
# with the engine's precise floating point; codegen (JIT) is never linked. LUA_USE_LONGJMP makes VM
# errors unwind with longjmp instead of C++ exceptions, so the -fno-exceptions engine can call it.
# The compiler throws internally (luau_compile catches); it is meant for the cook, not the player.
if(MCE_WITH_SCRIPT)
    set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
    set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(luau
        GIT_REPOSITORY https://github.com/luau-lang/luau.git
        GIT_TAG        0.742
        GIT_SHALLOW    TRUE
        SYSTEM
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(luau)
    target_compile_definitions(Luau.VM PUBLIC LUA_USE_LONGJMP=1)
    foreach(_luau_target Luau.VM Luau.Compiler Luau.Ast)
        if(MSVC)
            target_compile_options(${_luau_target} PRIVATE /fp:precise /W0)
        else()
            target_compile_options(${_luau_target} PRIVATE -ffp-contract=off -w)
        endif()
    endforeach()
endif()

# ---- SPIRV-Cross (macOS only) ---------------------------------------------------------------
# Declared in cmake/macos/MetalDependencies.cmake and pulled in by src/engine/rhi_metal (ADR-0028).
