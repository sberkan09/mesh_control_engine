# Needs a display and a GL 4.5 driver (CI: Xvfb + Mesa llvmpipe), so it only registers with
# MCE_GPU_TESTS. It always builds, so the GL backend compiles on every CI leg.
find_package(Threads REQUIRED)
add_executable(test_rhi_gl test_rhi_gl.cpp)
target_link_libraries(test_rhi_gl PRIVATE mce::mce_rhi_gl mce::doctest_main Threads::Threads)
mce_apply_compile_policy(test_rhi_gl)
if(MCE_GPU_TESTS)
    add_test(NAME test_rhi_gl COMMAND test_rhi_gl)
    set_tests_properties(test_rhi_gl PROPERTIES LABELS gpu)
endif()
