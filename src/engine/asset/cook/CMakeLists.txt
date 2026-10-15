# Third-party implementation TU: compiled without the engine warning policy.
add_library(mce_stb STATIC src/stb_impl.c)
target_link_libraries(mce_stb PUBLIC mce::stb_headers)
if(MSVC)
    target_compile_options(mce_stb PRIVATE /W0)
else()
    target_compile_options(mce_stb PRIVATE -w)
endif()

mce_add_library(mce_asset_cook UMBRELLA
    SOURCES
        src/audio.cpp
        src/cook.cpp
        src/cook_db.cpp
        src/diagnostics.cpp
        src/envmap_importer.cpp
        src/font_importer.cpp
        src/fs_util.cpp
        src/image.cpp
        src/gltf_importer.cpp
        src/importer_registry.cpp
        src/input_map_importer.cpp
        src/json.cpp
        src/material_importer.cpp
        src/mod_project.cpp
        src/particle_importer.cpp
        src/spritesheet_importer.cpp
        src/animgraph_importer.cpp
        src/terrain_importer.cpp
        src/registry.cpp
        src/scene_importer.cpp
        src/scene_writer.cpp
        src/sequence_importer.cpp
        src/timeline_importer.cpp
        src/shader_importer.cpp
        src/store_importer.cpp
        src/config_importer.cpp
        src/services_importer.cpp
        src/string_importer.cpp
        src/string.cpp
        src/texture_importer.cpp
        src/tilemap_importer.cpp
        src/ui_importer.cpp
        src/wav_importer.cpp
    PUBLIC_DEPS
        mce::mce_asset
    PRIVATE_DEPS
        mce::mce_input
        mce::mce_monetize
        mce::mce_liveops
        mce::mce_loc
        mce::mce_ui
        mce_stb
        mce_meshopt
        mce_astcenc
        glslang::glslang
        glslang::glslang-default-resource-limits)
