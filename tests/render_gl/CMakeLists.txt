# mce_render on a real GL 4.5 driver (CI: Xvfb + Mesa llvmpipe): pixel probes of the 2D path.
# Always builds; registers only with MCE_GPU_TESTS.
find_package(Threads REQUIRED)
add_executable(test_render_gl test_render_gl.cpp)
target_link_libraries(test_render_gl PRIVATE mce::mce_render mce::mce_rhi_gl mce::doctest_main Threads::Threads)
mce_apply_compile_policy(test_render_gl)
if(MCE_GPU_TESTS)
    add_test(NAME test_render_gl COMMAND test_render_gl)
    set_tests_properties(test_render_gl PROPERTIES LABELS gpu)
endif()
