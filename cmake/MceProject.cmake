# mce_add_project(): the build of one project made from the project template (assets.md §4.4,
# ROADMAP M8; scripts/new_project.py writes the call). For a project <game> it defines:
#
#   <game>_game     the game module lib<game> (gameplay + the MCE_GAME_MODULE entry), where game
#                   modules are supported; elsewhere those sources link into the host
#   <game>          the thin host: mce::player::run with the pack and module beside it, the build
#                   tree's as fallbacks (mce/player/player.h)
#   <game>_editor   mce_editor with the game linked in (editor builds): its components in the
#                   inspector and the in-editor cook, its install() in play-in-editor. The game's
#                   header must be <game>.h declaring <game>::install (the template's shape)
#   <game>_cook     mce_cook with the game's reflected components linked in (TYPES), so scenes that
#                   use them cook; the build runs it over a copy of assets/ into <build>/project/assets.mce
#   <game>_mcp      the editor MCP server (mce_mcp, editor-mcp.md) with the game's reflected
#                   components (TYPES) and systems (GAME_SOURCES) linked in, so an MCP client can
#                   author scenes that use them and run_headless plays them as the host does:
#                   <game>_mcp --project projects/<name> --scene scenes/main.mcscene
#   <game>_headless_300 (tests)  the host headless for 300 ticks: module loaded, scene played
#
#   mce_add_project(<game> TITLE "<window title>" GAME_SOURCES ... MODULE_ENTRY game/module.cpp
#                   TYPES game/types.cpp HOST host/main.cpp [INCLUDE_DIRS ...] [DEPS ...] [HOST_DEPS ...]
#                   [STORY])
#
# DEPS: optional engine libraries the game's sources use, linked wherever those sources are
# compiled (the module, or the host where modules are not supported, and the project editor and
# MCP server): mce::mce_net, the opt-in networking module (ADR-0035), and mce::mce_kit_novel, the
# visual-novel kit.
#
# HOST_DEPS: optional libraries the host links (game UI layers it adds to the player, mce/ui/ui.h
# ui::Layer): mce::mce_kit_novel_ui, the NPC dialogue box.
#
# STORY: the project is a novel (mce new --template novel): the build compiles story/*.story with
# story/story.json into the header story.json names ("output") before the game's sources, with
# mce story compile (tools/story/story_compile.py; the SDK's MCE_STORY_COMPILE).
#
# The per-project tools above compile the game in; where game modules are supported the stock
# tools load the module instead and need no project-specific build:
#   mce_editor -module <build>/projects/<name>/lib<game>.so -project projects/<name>
#   mce_mcp --module <that .so> --project projects/<name>
#   mce_cook --module <that .so> projects/<name> --pack
# (The tools and their libraries are UMBRELLA targets, cmake/MceTarget.cmake, so the module shares
# their engine.) scripts/ship.py --project <dir> packages the host, the module and the pack into
# the ship zip.
#
# Against an installed engine (MCE_SDK, set by MCEConfig.cmake; the project is its own top-level
# CMake project with find_package(MCE)) only <game>_game, <game> and <game>_content exist: the
# installed mce_cook cooks the content with the module loaded, and the installed mce_editor /
# mce_mcp open the project the same way (`mce editor`, `mce mcp`, `mce cook` in the mce CLI).
function(mce_add_project game)
    cmake_parse_arguments(ARG "STORY" "TITLE;MODULE_ENTRY;HOST" "GAME_SOURCES;TYPES;INCLUDE_DIRS;DEPS;HOST_DEPS" ${ARGN})
    if(NOT ARG_TITLE)
        set(ARG_TITLE "${game}")
    endif()
    set(src "${CMAKE_CURRENT_SOURCE_DIR}")
    set(includes)
    foreach(dir IN LISTS ARG_INCLUDE_DIRS)
        list(APPEND includes "${src}/${dir}")
    endforeach()
    # The DEPS' headers, for the targets that include them without linking them (the cook's TYPES,
    # a host's mce/kit/novel_host.h).
    set(dep_includes)
    foreach(dep IN LISTS ARG_DEPS)
        if(TARGET ${dep})
            get_target_property(dep_real ${dep} ALIASED_TARGET)
            if(NOT dep_real)
                set(dep_real ${dep})
            endif()
            list(APPEND dep_includes $<TARGET_PROPERTY:${dep_real},INTERFACE_INCLUDE_DIRECTORIES>)
        endif()
    endforeach()

    # The story: compiled into its header whenever a .story file or the manifest changes.
    set(story_header "")
    if(ARG_STORY)
        if(MCE_SDK)
            set(story_compiler "${MCE_STORY_COMPILE}")
        else()
            set(story_compiler "${PROJECT_SOURCE_DIR}/tools/story/story_compile.py")
        endif()
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(manifest "${src}/story/story.json")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest}")
        file(READ "${manifest}" manifest_text)
        string(JSON story_out ERROR_VARIABLE story_error GET "${manifest_text}" output)
        if(story_error)
            set(story_out "story_data.h")
        endif()
        set(story_header "${src}/${story_out}")
        file(GLOB story_files CONFIGURE_DEPENDS "${src}/story/*.story")
        add_custom_command(OUTPUT "${story_header}"
            COMMAND "${Python3_EXECUTABLE}" "${story_compiler}" "${src}"
            DEPENDS "${story_compiler}" "${manifest}" ${story_files}
            COMMENT "mce story compile: ${game}"
            VERBATIM)
        add_custom_target(${game}_story DEPENDS "${story_header}")
    endif()

    # The project's cook: the engine's component types plus the game's. Against the SDK the
    # installed mce_cook loads the game module instead (below).
    if(NOT MCE_SDK)
        add_executable(${game}_cook "${PROJECT_SOURCE_DIR}/tools/cook/main.cpp" "${PROJECT_SOURCE_DIR}/tools/cook/engine_components.cpp"
                                    ${ARG_TYPES})
        target_include_directories(${game}_cook PRIVATE ${includes})
        # The game's TYPES may include an opt-in library's components (mce/kit/novel_components.h):
        # its headers only, the cook does not link it.
        target_include_directories(${game}_cook PRIVATE ${dep_includes})
        mce_engine_deps(cook_deps mce::mce_asset_cook mce::mce_gameplay_kit mce::mce_physics2d mce::mce_audio mce::mce_ecs_scene)
        target_link_libraries(${game}_cook PRIVATE ${cook_deps})
        mce_apply_compile_policy(${game}_cook)
    endif()

    # The start scene the host plays when -scene is not given: <name>.mceproj's start_scene
    # (re-read when the file changes).
    set(start_scene "")
    file(GLOB mceproj "${src}/*.mceproj")
    if(mceproj)
        list(GET mceproj 0 mceproj)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${mceproj}")
        file(READ "${mceproj}" mceproj_text)
        string(JSON start_scene ERROR_VARIABLE start_scene_error GET "${mceproj_text}" start_scene)
        if(start_scene_error)
            set(start_scene "")
        endif()
    endif()

    # The game and its host.
    if(MCE_GAME_MODULES_SUPPORTED)
        mce_add_game_module(${game}_game SOURCES ${ARG_GAME_SOURCES} ${ARG_MODULE_ENTRY})
        set_target_properties(${game}_game PROPERTIES OUTPUT_NAME "${game}")
        target_include_directories(${game}_game PRIVATE ${includes})
        target_link_libraries(${game}_game PRIVATE ${ARG_DEPS})
        add_executable(${game} ${ARG_HOST})
        target_include_directories(${game} PRIVATE ${dep_includes})
        target_compile_definitions(${game} PRIVATE MCE_PROJECT_MODULE="$<TARGET_FILE:${game}_game>"
                                                   MCE_PROJECT_MODULE_NAME="$<TARGET_FILE_NAME:${game}_game>")
        add_dependencies(${game} ${game}_game)
    else()
        add_executable(${game} ${ARG_HOST} ${ARG_GAME_SOURCES} ${ARG_MODULE_ENTRY})
        target_compile_definitions(${game} PRIVATE MCE_PROJECT_STATIC_MODULE=1)
        target_include_directories(${game} PRIVATE ${includes})
        target_link_libraries(${game} PRIVATE ${ARG_DEPS})
    endif()
    # Content: the committed assets/ tree cooked into a shipping pack at build time, by the
    # project's cook or (SDK) the installed cook with the game module loaded.
    set(project_dir "${CMAKE_CURRENT_BINARY_DIR}/project")
    set(pack "${project_dir}/assets.mce")
    file(GLOB_RECURSE content CONFIGURE_DEPENDS "${src}/assets/*")
    if(MCE_SDK)
        set(cook_command "${MCE_COOK_EXE}" --module "$<TARGET_FILE:${game}_game>")
        set(cook_depends ${game}_game)
    else()
        set(cook_command "$<TARGET_FILE:${game}_cook>")
        set(cook_depends ${game}_cook)
    endif()
    add_custom_command(
        OUTPUT "${pack}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${project_dir}/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${src}/assets" "${project_dir}/assets"
        COMMAND ${cook_command} "${project_dir}" --pack --quiet ${MCE_COOK_TEXTURE_ARGS}
        DEPENDS ${cook_depends} ${content}
        COMMENT "Cooking ${game} content"
        VERBATIM)
    add_custom_target(${game}_content ALL DEPENDS "${pack}")

    target_link_libraries(${game} PRIVATE mce::mce_player_runtime ${ARG_HOST_DEPS})
    target_compile_definitions(${game} PRIVATE MCE_PROJECT_NAME="${game}" MCE_PROJECT_TITLE="${ARG_TITLE}"
                                               MCE_PROJECT_PACK="${pack}" MCE_PROJECT_START_SCENE="${start_scene}")
    add_dependencies(${game} ${game}_content)
    set_target_properties(${game} PROPERTIES MCE_SAMPLE_PACK "${pack}")
    mce_apply_compile_policy(${game})
    if(MCE_SDK)
        # Shipped: the module, the pack and libmce_engine sit beside the executable ($ORIGIN first);
        # in the build tree the engine is found in the SDK.
        set_target_properties(${game} ${game}_game PROPERTIES BUILD_RPATH_USE_ORIGIN ON
                              BUILD_RPATH "${MCE_ORIGIN_RPATH};${MCE_LIB_DIR}")
        return()
    endif()

    # The project's editor: the game's types and systems linked into mce_editor's main.
    if(TARGET mce_editor_layer)
        set(hook "${CMAKE_CURRENT_BINARY_DIR}/${game}_editor_game.cpp")
        file(CONFIGURE OUTPUT "${hook}" CONTENT "// Generated by mce_add_project: hands ${game}::install to play-in-editor.
#include \"${game}.h\"
#include \"mce/editor/play_session.h\"

namespace {
const bool kRegistered = (mce::editor::set_play_game_install(&${game}::install), true);
}  // namespace
" @ONLY)
        add_executable(${game}_editor "${PROJECT_SOURCE_DIR}/src/editor/app/main.cpp" ${ARG_GAME_SOURCES} "${hook}")
        target_include_directories(${game}_editor PRIVATE ${includes})
        mce_engine_deps(editor_deps mce::mce_editor_layer mce::mce_rhi_default mce::mce_module_host mce::mce_input ${ARG_DEPS})
        target_link_libraries(${game}_editor PRIVATE ${editor_deps})
        mce_apply_compile_policy(${game}_editor)
    endif()

    # The project's MCP server: mce_mcp's stdio main plus the game's types and systems (editor
    # tooling; never linked by a runtime target). The game's install() goes to run_headless through
    # the same hook as play-in-editor, so its runs hash like the host's.
    if(TARGET mce_mcp_core)
        set(mcp_hook "${CMAKE_CURRENT_BINARY_DIR}/${game}_mcp_game.cpp")
        file(CONFIGURE OUTPUT "${mcp_hook}" CONTENT "// Generated by mce_add_project: hands ${game}::install to run_headless.
#include \"${game}.h\"
#include \"mce/editor/play_session.h\"

namespace {
const bool kRegistered = (mce::editor::set_play_game_install(&${game}::install), true);
}  // namespace
" @ONLY)
        add_executable(${game}_mcp "${PROJECT_SOURCE_DIR}/src/editor/mcp/app/main.cpp"
                                   "${PROJECT_SOURCE_DIR}/src/editor/mcp/app/engine_components.cpp" ${ARG_TYPES}
                                   ${ARG_GAME_SOURCES} "${mcp_hook}")
        target_include_directories(${game}_mcp PRIVATE ${includes})
        mce_engine_deps(mcp_deps mce::mce_mcp_core mce::mce_module_host ${ARG_DEPS})
        target_link_libraries(${game}_mcp PRIVATE ${mcp_deps})
        mce_apply_compile_policy(${game}_mcp)
    endif()

    # Every target that compiles the game's sources waits for the story's header.
    if(ARG_STORY)
        foreach(t ${game}_game ${game} ${game}_editor ${game}_mcp)
            if(TARGET ${t})
                add_dependencies(${t} ${game}_story)
            endif()
        endforeach()
    endif()

    if(MCE_BUILD_TESTS)
        add_test(NAME ${game}_headless_300 COMMAND ${game} -headless 1 -ticks 300)
        set_tests_properties(${game}_headless_300 PROPERTIES PASS_REGULAR_EXPRESSION "300 ticks, [0-9]+ entities, state_hash")
        if(MCE_GPU_TESTS)
            add_test(NAME ${game}_gpu COMMAND ${game} -frames 60 -platform.error_box 0 -r.vsync 0)
            set_tests_properties(${game}_gpu PROPERTIES LABELS gpu)
            if(TARGET ${game}_editor)
                # The project editor opens the project, cooks it and plays the scene with the game's systems.
                add_test(NAME ${game}_editor_gpu
                         COMMAND ${game}_editor -project "${src}" -frames 60 -editor.play 1 -editor.ini ""
                                 -platform.error_box 0 -r.vsync 0)
                set_tests_properties(${game}_editor_gpu PROPERTIES LABELS gpu)
            endif()
        endif()
        # The module host maps lib<game>_loaded_<pid>_N copies beside the module; one test at a time
        # keeps the project folder quiet under -j.
        get_property(tests DIRECTORY PROPERTY TESTS)
        set_tests_properties(${tests} PROPERTIES RESOURCE_LOCK ${game}_module)
    endif()
endfunction()
