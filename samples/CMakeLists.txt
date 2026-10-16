# Sample ladder (ROADMAP §2): each sample is one milestone's running demo.

# mce_sample_pack(<target>): cooks the sample's assets/ directory into <build>/project/assets.mce
# with mce_cook at build time (sidecars are committed next to the sources) and tells the target
# where the dev pack is (MCE_SAMPLE_DEV_PACK). scripts/ship.py copies that pack next to the
# executable, where the sample looks first.
function(mce_sample_pack target)
    set(project_dir "${CMAKE_CURRENT_BINARY_DIR}/project")
    set(pack "${project_dir}/assets.mce")
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
    if(TARGET mce_cook)
        set(cook $<TARGET_FILE:mce_cook>)
        set(cook_dep mce_cook)
    elseif(MCE_HOST_COOK)
        set(cook "${MCE_HOST_COOK}")  # cross build (cmake/MceMobile.cmake): the host's cook
        set(cook_dep "${MCE_HOST_COOK}")
    else()
        message(FATAL_ERROR "${target}: a cross build needs -DMCE_HOST_COOK=<host mce_cook> to cook its pack")
    endif()
    add_custom_command(
        OUTPUT "${pack}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${project_dir}/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${project_dir}/assets"
        COMMAND ${cook} "${project_dir}" --pack --quiet ${MCE_COOK_TEXTURE_ARGS}
        DEPENDS ${cook_dep} ${sources}
        COMMENT "Cooking ${target} content"
        VERBATIM)
    set(outputs "${pack}")
    if(MCE_WEB)
        # The browser fetches assets.mce from next to the page (resolve_shipped_path): copy it there.
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/assets.mce"
            COMMAND ${CMAKE_COMMAND} -E copy "${pack}" "${CMAKE_CURRENT_BINARY_DIR}/assets.mce"
            DEPENDS "${pack}"
            VERBATIM)
        list(APPEND outputs "${CMAKE_CURRENT_BINARY_DIR}/assets.mce")
    endif()
    add_custom_target(${target}_content DEPENDS ${outputs})
    add_dependencies(${target} ${target}_content)
    target_compile_definitions(${target} PRIVATE MCE_SAMPLE_DEV_PACK="${pack}")
    set_target_properties(${target} PROPERTIES MCE_SAMPLE_PACK "${pack}")
endfunction()
if(MCE_WEB)
    # Browsers (M-WEB): the windowed samples that ship to the web. All but 01_triangle cook a pack,
    # which needs the host's mce_cook (MCE_HOST_COOK).
    set(_web_samples 01_triangle)
    if(MCE_HOST_COOK)
        list(APPEND _web_samples sprites_2d 03_platformer 04_diorama 04_cubes 05_ashen_court)
    endif()
    foreach(_sample IN LISTS _web_samples)
        add_subdirectory(${_sample})
    endforeach()
    return()
endif()
if(MCE_MOBILE)
    # Phones (M-MOBILE): the windowed samples that ship as apps; headless and tool demos stay desktop.
    foreach(_sample 01_triangle sprites_2d 03_platformer 04_diorama 04_cubes 05_ashen_court)
        add_subdirectory(${_sample})
    endforeach()
    return()
endif()
add_subdirectory(01_triangle)
add_subdirectory(02_boids)
add_subdirectory(physics2d_headless)
add_subdirectory(physics3d_headless)
if(MCE_WITH_SCRIPT)
    add_subdirectory(script_headless)  # ADR-0041 spike
endif()
add_subdirectory(sprites_2d)
add_subdirectory(ui_gallery)
add_subdirectory(03_platformer)
add_subdirectory(04_diorama)
add_subdirectory(04_cubes)
add_subdirectory(05_ashen_court)
add_subdirectory(06_showcase)
add_subdirectory(scene_demo)
add_subdirectory(07_gameplay3d)
add_subdirectory(ui_screens)
add_subdirectory(cinema_demo)
add_subdirectory(stream_demo)
add_subdirectory(audio_rooms)
add_subdirectory(extras_2d)
add_subdirectory(net_headless)
add_subdirectory(net_arena)
add_subdirectory(net_rollback)
add_subdirectory(shop_demo)
add_subdirectory(liveops_demo)
add_subdirectory(steam_demo)
add_subdirectory(ai_demo)
