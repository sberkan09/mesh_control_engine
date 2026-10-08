# Game-module loader tests (scripting.md §6.1/§6.2): a host linked against the mce_engine shared
# library loads synthetic modules built from test_game_module.cpp.
if(NOT MCE_GAME_MODULES_SUPPORTED)
    return()
endif()

mce_add_game_module(modtest_good SOURCES test_game_module.cpp)
target_compile_definitions(modtest_good PRIVATE MODTEST_VARIANT_GOOD)
mce_add_game_module(modtest_no_entry SOURCES test_game_module.cpp)
target_compile_definitions(modtest_no_entry PRIVATE MODTEST_VARIANT_NO_ENTRY)
mce_add_game_module(modtest_install_fail SOURCES test_game_module.cpp)
target_compile_definitions(modtest_install_fail PRIVATE MODTEST_VARIANT_INSTALL_FAIL)
mce_add_game_module(modtest_stale SOURCES test_game_module.cpp)
target_compile_definitions(modtest_stale PRIVATE MODTEST_VARIANT_GOOD MCE_ENGINE_ID=0x5157a1eull)

set(_modtest_modules modtest_good modtest_no_entry modtest_install_fail modtest_stale)
foreach(m ${_modtest_modules})
    target_include_directories(${m} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(${m} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/modules)
endforeach()

# The host links the umbrella only: no static engine library, no mce::doctest_main (it would
# bring a second mce_core). test_main.cpp is compiled in directly instead.
add_executable(test_module test_module.cpp ${PROJECT_SOURCE_DIR}/tests/test_main.cpp)
target_include_directories(test_module PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests)
target_link_libraries(test_module PRIVATE mce::mce_engine mce::doctest)
target_compile_definitions(test_module PRIVATE
    MODTEST_GOOD="$<TARGET_FILE:modtest_good>"
    MODTEST_NO_ENTRY="$<TARGET_FILE:modtest_no_entry>"
    MODTEST_INSTALL_FAIL="$<TARGET_FILE:modtest_install_fail>"
    MODTEST_STALE="$<TARGET_FILE:modtest_stale>"
    MODTEST_SCRATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}")
add_dependencies(test_module ${_modtest_modules})
mce_apply_compile_policy(test_module)
add_test(NAME test_module COMMAND test_module)
