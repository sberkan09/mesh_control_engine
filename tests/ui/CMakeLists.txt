mce_add_test(test_ui SOURCES test_ui.cpp DEPS mce::mce_ui)
mce_add_test(test_ui_layout SOURCES test_ui_layout.cpp DEPS mce::mce_ui)

# The menu through the real 2D path (GL 4.5; CI: Xvfb + llvmpipe). Always builds; registers with MCE_GPU_TESTS.
find_package(Threads REQUIRED)
add_executable(test_ui_gl test_ui_gl.cpp)
target_link_libraries(test_ui_gl PRIVATE mce::mce_ui mce::mce_platform mce::mce_rhi_default mce::doctest_main Threads::Threads)
mce_apply_compile_policy(test_ui_gl)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tmp")  # the layout screenshots (ui_layout_<w>x<h>.ppm)
target_compile_definitions(test_ui_gl PRIVATE MCE_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/tmp")
if(MCE_GPU_TESTS)
    add_test(NAME test_ui_gl COMMAND test_ui_gl)
    set_tests_properties(test_ui_gl PROPERTIES LABELS gpu)
endif()

# FR-UI-048: no ImGui symbol in the engine library or the shipped player (ImGui is editor-only).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET mce_engine AND TARGET mce_player AND CMAKE_NM)
    add_test(NAME ui_no_imgui_symbols
             COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} "-DFILES=$<TARGET_FILE:mce_engine>;$<TARGET_FILE:mce_player>"
                     -DPATTERN=ImGui -P ${PROJECT_SOURCE_DIR}/cmake/ScanSymbols.cmake)
endif()
mce_add_test(test_ui_screen_link SOURCES test_ui_screen_link.cpp DEPS mce::mce_ui)
