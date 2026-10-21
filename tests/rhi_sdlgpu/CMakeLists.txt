# mce_rhi_sdlgpu tests (ADR-0038). test_shader_spirv needs no GPU and always registers;
# test_rhi_sdlgpu needs a display and a Vulkan driver (CI: Xvfb + Mesa lavapipe), so it registers
# with MCE_GPU_TESTS like the GL one.
find_package(Threads REQUIRED)

mce_add_test(test_shader_spirv SOURCES test_shader_spirv.cpp DEPS mce::mce_rhi_sdlgpu)
# The translator's header is internal to the backend.
target_include_directories(test_shader_spirv PRIVATE "${PROJECT_SOURCE_DIR}/src/engine/rhi_sdlgpu/src")

add_executable(test_rhi_sdlgpu test_rhi_sdlgpu.cpp)
target_link_libraries(test_rhi_sdlgpu PRIVATE mce::mce_rhi_sdlgpu mce::doctest_main Threads::Threads)
mce_apply_compile_policy(test_rhi_sdlgpu)
if(MCE_GPU_TESTS)
    add_test(NAME test_rhi_sdlgpu COMMAND test_rhi_sdlgpu)
    set_tests_properties(test_rhi_sdlgpu PROPERTIES LABELS gpu ENVIRONMENT "MCE_RHI=sdlgpu")
endif()
