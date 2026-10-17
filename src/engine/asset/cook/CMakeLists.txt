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
        src/navmesh_importer.cpp
        src/hlod_importer.cpp
        src/roads_importer.cpp
        src/font_importer.cpp
        src/fs_util.cpp
        src/image.cpp
        src/gltf_importer.cpp
        src/importer_registry.cpp
        src/input_map_importer.cpp
        src/json.cpp
        src/material_importer.cpp
        src/mesh_build.cpp
        src/mod_project.cpp
        src/obj_importer.cpp
        src/particle_importer.cpp
        src/sound_set_importer.cpp
        src/spritesheet_importer.cpp
        src/animgraph_importer.cpp
        src/terrain_importer.cpp
        src/registry.cpp
        src/scene_importer.cpp
        src/scene_writer.cpp
        src/sequence_importer.cpp
        src/timeline_importer.cpp
        src/bt_importer.cpp
        src/skeleton2d_importer.cpp
        src/shader_importer.cpp
        src/store_importer.cpp
        src/config_importer.cpp
        src/services_importer.cpp
        src/string_importer.cpp
        src/string.cpp
        src/texture_bc.cpp
        src/texture_importer.cpp
        src/tilemap_importer.cpp
        src/ui_importer.cpp
        src/video_importer.cpp
        src/vorbis_encode.cpp
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
        mce_tinyexr
        mce_astcenc
        mce_bc7enc
        mce_vorbisenc
        glslang::glslang
        glslang::glslang-default-resource-limits)

# Luau scripts (M-SCRIPT 1): .luau -> CHNK_SCRIPT through mce_script_compiler, where the build has
# scripting. The compiler lives here (and in the editor), never in the player.
if(MCE_WITH_SCRIPT)
    target_sources(mce_asset_cook PRIVATE src/script_importer.cpp)
    target_link_libraries(mce_asset_cook PRIVATE mce::mce_script_compiler)
endif()
