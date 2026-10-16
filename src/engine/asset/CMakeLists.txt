# mce_asset (L2): .mce container reader/writer, GUIDs, cooked chunk formats, runtime asset
# registry (handles, refcount, deferred unload, hot-reload commit), IGpuUploadSink (fix F6).
# No third-party code: this is what shipped runtimes link (FR-AST-010).

mce_add_library(mce_asset
    SOURCES
        src/asset_system.cpp
        src/container.cpp
        src/file_io.cpp
        src/formats.cpp
        src/guid.cpp
        src/mods.cpp
        src/vorbis.cpp
        src/xxhash.cpp
    PUBLIC_DEPS
        mce::mce_core
        mce::mce_refl)
# stb_vorbis (ADR-0044): the runtime decoder for Ogg Vorbis clips, one third-party TU folded into
# mce_asset and compiled without the engine warning policy; only vorbis.cpp includes it.
target_sources(mce_asset PRIVATE src/stb_vorbis_impl.c)
target_include_directories(mce_asset SYSTEM PRIVATE "${stb_SOURCE_DIR}")
if(MSVC)
    set_source_files_properties(src/stb_vorbis_impl.c PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(src/stb_vorbis_impl.c PROPERTIES COMPILE_OPTIONS "-w")
endif()
# The async loader thread (AssetSystem::request_async).
find_package(Threads REQUIRED)
target_link_libraries(mce_asset PUBLIC Threads::Threads)

# mce_asset_cook: the cook library (scan, registry, importers, dependency graph, cook DB,
# content-addressed outputs, pack). Backs mce_cook and the editor's cook button. Cook-time-only
# third-party code (stb) is confined to this target and must never reach a runtime target.
# A cross build (phones, the browser) cooks with the host's mce_cook (MCE_HOST_COOK), so it has no cook library.
if(NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(cook)
endif()
