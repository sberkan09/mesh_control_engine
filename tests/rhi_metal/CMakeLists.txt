# mce_rhi_metal tests (macOS only). test_shader_cross needs no GPU and always registers;
# test_rhi_metal needs a logged-in window server, so it registers with MCE_GPU_TESTS like the GL one.
find_package(Threads REQUIRED)

mce_add_test(test_shader_cross SOURCES test_shader_cross.cpp DEPS mce::mce_rhi_metal)
# The translator's header is internal to the backend.
target_include_directories(test_shader_cross PRIVATE "${PROJECT_SOURCE_DIR}/src/engine/rhi_metal/src")

add_executable(test_rhi_metal test_rhi_metal.cpp)
target_link_libraries(test_rhi_metal PRIVATE mce::mce_rhi_metal mce::doctest_main Threads::Threads)
mce_apply_compile_policy(test_rhi_metal)
if(MCE_GPU_TESTS)
    add_test(NAME test_rhi_metal COMMAND test_rhi_metal)
    set_tests_properties(test_rhi_metal PROPERTIES LABELS gpu)
endif()
