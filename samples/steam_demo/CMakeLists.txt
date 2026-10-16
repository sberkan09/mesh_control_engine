# M-STEAM's demo (SBA-111): an editor project (assets/, no code) played by the stock mce_player on Steam.
# A scene with a RichPresence and a .mcservices whose achievements and leaderboard carry Steam names. CI
# plays it against the stand-in steam_api (tests/platform/steam_stub.cpp): Steam Input, the overlay
# holding the game, rich presence, achievements, a leaderboard and a Steam Cloud save in one run, the
# state hash unchanged by Steam, then the build laid out as a Steam depot and started the way Steam
# starts it. Desktop only; Valve's client is SBA-112.
set(project_dir "${CMAKE_CURRENT_BINARY_DIR}/project")
set(pack "${project_dir}/assets.mce")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
add_custom_command(
    OUTPUT "${pack}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${project_dir}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${project_dir}/assets"
    COMMAND $<TARGET_FILE:mce_cook> "${project_dir}" --pack --quiet
    DEPENDS mce_cook ${sources}
    COMMENT "Cooking steam_demo content"
    VERBATIM)
add_custom_target(steam_demo_content ALL DEPENDS "${pack}")
add_dependencies(steam_demo_content mce_player)

# The stand-in steam_api is built by tests/platform, added after the samples.
if(MCE_BUILD_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if(TARGET mce_engine)
        set(_engine $<TARGET_FILE:mce_engine>)
    else()
        set(_engine -)
    endif()
    add_test(NAME sample_steam_demo
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/steam_demo_test.py"
                     --player $<TARGET_FILE:mce_player> --engine ${_engine} --stub $<TARGET_FILE:mce_steam_stub>
                     --pack "${pack}" --work "${CMAKE_CURRENT_BINARY_DIR}/run")
endif()
