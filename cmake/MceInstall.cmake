# The installable engine (the SDK): `cmake --install <build> --prefix <dir>` lays out
#
#   bin/        mce_player, mce_editor, mce_mcp, mce_cook, mce_devhost and the `mce` command
#   lib/        libmce_engine (the umbrella every tool and game module shares),
#               libmce_player_runtime (what a project's host links), libmce_net (the opt-in
#               networking module, when built with MCE_WITH_NET), libmce_kit_novel (the opt-in
#               visual-novel kit and dialogue VM) and libmce_kit_novel_ui (its NPC dialogue box)
#   include/    the engine headers (every src/**/include), the generated mce/engine_version.h
#               (MCE_ENGINE_ID: modules built against this SDK match this engine) and spdlog's
#               headers, which mce/log.h includes
#   lib/cmake/MCE/   MCEConfig.cmake (find_package(MCE)): imported targets mce::mce_engine,
#               mce::mce_player_runtime, mce::mce_module_api, the tool paths, and the same
#               MceTarget / MceProject / MceShip helpers the engine tree uses
#   share/mce/  the project templates (project, novel, script), new_project.py, story_compile.py, VERSION and examples/Kindle, examples/Undelivered, examples/Room3D (projects/)
#   share/doc/mce/   the user guide, the engine and architecture pages, ARCHITECTURE.md, REQUIREMENTS.md
#
# A project made from the template builds against it with find_package(MCE) (its CMakeLists.txt),
# or through `mce new` / `mce build` / `mce play` / `mce editor` / `mce mcp` / `mce ship`
# (tools/mce_cli/mce.py). Only where game modules are supported: the SDK's tools load the game
# module, which the Windows build cannot do yet.

if(NOT MCE_GAME_MODULES_SUPPORTED)
    return()
endif()
include(GNUInstallDirs)

set(_mce_sdk_tools mce_player mce_cook)
foreach(t mce_editor mce_mcp mce_devhost)
    if(TARGET ${t})
        list(APPEND _mce_sdk_tools ${t})
    endif()
endforeach()
# The tools find libmce_engine beside themselves (a shipped folder) or in ../lib (the SDK).
set_target_properties(${_mce_sdk_tools} PROPERTIES INSTALL_RPATH "${CMAKE_BUILD_RPATH};${CMAKE_BUILD_RPATH}/../${CMAKE_INSTALL_LIBDIR}")
install(TARGETS ${_mce_sdk_tools} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS mce_engine LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS mce_player_runtime ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(TARGET mce_net)
    # The opt-in networking module (ADR-0035): a static archive a game links on request.
    install(TARGETS mce_net ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    set(MCE_SDK_NET_FILE "$<TARGET_FILE_NAME:mce_net>")
else()
    set(MCE_SDK_NET_FILE "")
endif()
# Scene scripts (M-SCRIPT): the player runtime links the bytecode runner and Luau's VM (never its
# compiler), so a project host built against the SDK runs ScriptSystems like mce_player.
if(TARGET mce_script)
    install(TARGETS mce_script Luau.VM ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    set(MCE_SDK_SCRIPT_FILE "$<TARGET_FILE_NAME:mce_script>")
    set(MCE_SDK_LUAU_VM_FILE "$<TARGET_FILE_NAME:Luau.VM>")
    set(MCE_SDK_RUNTIME_LINKS "mce::mce_script;mce::mce_engine")
else()
    set(MCE_SDK_SCRIPT_FILE "")
    set(MCE_SDK_LUAU_VM_FILE "")
    set(MCE_SDK_RUNTIME_LINKS "mce::mce_engine")
endif()
# The opt-in visual-novel kit (M-NOVEL), the same way.
install(TARGETS mce_kit_novel mce_kit_novel_ui ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
set(MCE_SDK_NOVEL_FILE "$<TARGET_FILE_NAME:mce_kit_novel>")
set(MCE_SDK_NOVEL_UI_FILE "$<TARGET_FILE_NAME:mce_kit_novel_ui>")

# Headers: every engine, module and player include tree, then the generated one.
file(GLOB _mce_include_dirs LIST_DIRECTORIES true
     "${PROJECT_SOURCE_DIR}/src/engine/*/include" "${PROJECT_SOURCE_DIR}/src/engine/asset/cook/include"
     "${PROJECT_SOURCE_DIR}/src/module/include" "${PROJECT_SOURCE_DIR}/src/player/include")
foreach(dir IN LISTS _mce_include_dirs)
    install(DIRECTORY "${dir}/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.h" PATTERN "*.inl")
endforeach()
install(DIRECTORY "${MCE_GENERATED_INCLUDE_DIR}/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.h")
install(DIRECTORY "${spdlog_SOURCE_DIR}/include/spdlog" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# The CMake package.
set(_mce_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/MCE")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(MCE_SDK_SPDLOG_LEVEL "SPDLOG_LEVEL_TRACE")
else()
    set(MCE_SDK_SPDLOG_LEVEL "SPDLOG_LEVEL_INFO")
endif()
set(MCE_SDK_ENGINE_FILE "$<TARGET_FILE_NAME:mce_engine>")
set(MCE_SDK_RUNTIME_FILE "$<TARGET_FILE_NAME:mce_player_runtime>")
set(MCE_SDK_EXE_SUFFIX "${CMAKE_EXECUTABLE_SUFFIX}")
configure_file("${PROJECT_SOURCE_DIR}/cmake/MCEConfig.cmake.in" "${PROJECT_BINARY_DIR}/sdk/MCEConfig.cmake.gen" @ONLY)
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/sdk/MCEConfig.cmake" INPUT "${PROJECT_BINARY_DIR}/sdk/MCEConfig.cmake.gen")
include(CMakePackageConfigHelpers)
write_basic_package_version_file("${PROJECT_BINARY_DIR}/sdk/MCEConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${PROJECT_BINARY_DIR}/sdk/MCEConfig.cmake" "${PROJECT_BINARY_DIR}/sdk/MCEConfigVersion.cmake"
              "${PROJECT_SOURCE_DIR}/cmake/MceTarget.cmake" "${PROJECT_SOURCE_DIR}/cmake/MceProject.cmake"
              "${PROJECT_SOURCE_DIR}/cmake/MceShip.cmake" "${PROJECT_SOURCE_DIR}/cmake/ship.rc.in"
        DESTINATION ${_mce_cmake_dir})

# The project template, its generator and the mce command.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/templates/project" "${PROJECT_SOURCE_DIR}/templates/novel" "${PROJECT_SOURCE_DIR}/templates/script"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/mce/templates)
install(PROGRAMS "${PROJECT_SOURCE_DIR}/scripts/new_project.py" "${PROJECT_SOURCE_DIR}/tools/story/story_compile.py"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/mce/scripts)
file(WRITE "${PROJECT_BINARY_DIR}/sdk/VERSION" "${PROJECT_VERSION}\n${MCE_ENGINE_DESCRIBE}\n")
install(FILES "${PROJECT_BINARY_DIR}/sdk/VERSION" DESTINATION ${CMAKE_INSTALL_DATADIR}/mce)
install(PROGRAMS "${PROJECT_SOURCE_DIR}/tools/mce_cli/mce.py" DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME mce)
install(FILES "${PROJECT_SOURCE_DIR}/tools/mce_cli/mce_steam.py" DESTINATION ${CMAKE_INSTALL_BINDIR})  # mce ship --steam
install(PROGRAMS "${PROJECT_SOURCE_DIR}/install.sh" DESTINATION ${CMAKE_INSTALL_DATADIR}/mce)  # mce update (release installs)

# The example projects (`mce example <Name>` copies one out): sources and content, no build outputs.
# Kindle is the 2D platformer reference, Undelivered the light-novel one, Room3D the 3D kit one.
foreach(_mce_example Kindle Undelivered Room3D)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/projects/${_mce_example}" DESTINATION ${CMAKE_INSTALL_DATADIR}/mce/examples
            PATTERN "cooked" EXCLUDE PATTERN "build" EXCLUDE PATTERN "dist" EXCLUDE
            PATTERN "assets.mce" EXCLUDE PATTERN "__pycache__" EXCLUDE)
endforeach()

# The worked samples, as source to read (the MCP server serves them as mce://docs/samples/...):
# code, scenes and content, no build outputs. They build inside the engine tree, not from the SDK.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/samples/" DESTINATION ${CMAKE_INSTALL_DATADIR}/mce/samples
        PATTERN "cooked" EXCLUDE PATTERN "build" EXCLUDE PATTERN "dist" EXCLUDE
        PATTERN "assets.mce" EXCLUDE PATTERN "__pycache__" EXCLUDE)

# The content library (library_search / library_add): library.json and its own files. Its entries
# name files under share/mce/samples and share/mce/examples, installed above.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/library/" DESTINATION ${CMAKE_INSTALL_DATADIR}/mce/library)

# The documentation a project author reads (docs/user-guide/README.md is its table of contents).
set(_mce_doc_dir "${CMAKE_INSTALL_DATADIR}/doc/mce")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/docs/user-guide" "${PROJECT_SOURCE_DIR}/docs/engine"
                  "${PROJECT_SOURCE_DIR}/docs/architecture"
        DESTINATION ${_mce_doc_dir} FILES_MATCHING PATTERN "*.md" PATTERN "*.png" PATTERN "*.svg")
install(FILES "${PROJECT_SOURCE_DIR}/docs/ARCHITECTURE.md" "${PROJECT_SOURCE_DIR}/docs/REQUIREMENTS.md"
        DESTINATION ${_mce_doc_dir})
