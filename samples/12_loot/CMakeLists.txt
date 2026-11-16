# 12_loot: M-INVENTORY's demo, a data-only game the stock mce_player plays (assets/ + one Luau script, no C++).
# A hero, a chest and a goblin on a ground box; assets/items/loot.mcitems holds eight items, two loot tables and two crafting recipes. The script
# gives and equips items, drinks a potion (which heals) and wears a helm (armor, max health), opens the chest, kills the goblin, drops potions and crafts a tonic and a potion pack at the workbench, logging "inv:" lines.
# `mce_player -pack <build>/samples/12_loot/project/assets.mce -input samples/12_loot/input.txt`
if(NOT TARGET mce_cook OR NOT MCE_WITH_SCRIPT)
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
    COMMENT "Cooking 12_loot content"
    VERBATIM)
add_custom_target(loot_sample_content ALL DEPENDS "${pack}")
add_dependencies(loot_sample_content mce_player)

if(MCE_BUILD_TESTS)
    # The player run of the cooked pack with the sample's script and input; the numbers are in loot_test.cmake.
    add_test(NAME sample_loot_headless
             COMMAND ${CMAKE_COMMAND} -DMCE_PLAYER=$<TARGET_FILE:mce_player> -DPACK=${pack}
                     -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/input.txt -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/headless
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/loot_test.cmake)
endif()
