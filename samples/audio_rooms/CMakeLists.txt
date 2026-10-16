# audio_rooms: M-AUDIO2's demo, a data-only project the stock mce_player plays. A listener walks
# through three rooms on a timeline. Footsteps come from a sound set with pitch variation. The
# middle room sends the SFX bus into a reverb, and a cart drives past with Doppler. The music
# ducks under a voice line on the Voice bus, a snapshot muffles the mix for a moment, and a
# machine behind two walls is occluded until the listener is in its room. The music, voice and
# hum are cooked to Ogg Vorbis. tools/gen_content.py makes the sounds; -audio_record renders the
# run offline.
if(NOT TARGET mce_cook)
    return()
endif()
set(project_dir "${CMAKE_CURRENT_BINARY_DIR}/project")
set(pack "${project_dir}/assets.mce")
file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
add_custom_command(
    OUTPUT "${pack}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${project_dir}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${project_dir}/assets"
    COMMAND $<TARGET_FILE:mce_cook> "${project_dir}" --pack --quiet
    DEPENDS mce_cook ${sources}
    COMMENT "Cooking audio_rooms content"
    VERBATIM)
add_custom_target(audio_rooms_content ALL DEPENDS "${pack}")
add_dependencies(audio_rooms_content mce_player)

if(MCE_BUILD_TESTS)
    # Headless, rendered offline: two runs give the same samples, and the mix does what the scene
    # asks (player_test.cmake measures the rooms in the recording).
    add_test(NAME audio_rooms_player
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack}
                     -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/player_test -P ${CMAKE_CURRENT_SOURCE_DIR}/player_test.cmake)
endif()
