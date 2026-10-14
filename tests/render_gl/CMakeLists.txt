# mce_render on a real GL 4.5 driver (CI: Xvfb + Mesa llvmpipe): pixel probes of the 2D and 3D paths,
# run again on mce_rhi_sdlgpu (Vulkan, lavapipe) as test_render_sdlgpu.
# Always builds; registers only with MCE_GPU_TESTS.
find_package(Threads REQUIRED)
add_executable(test_render_gl test_render_gl.cpp test_render3d_gl.cpp)
target_link_libraries(test_render_gl PRIVATE mce::mce_render mce::mce_rhi_default mce::doctest_main Threads::Threads)
mce_apply_compile_policy(test_render_gl)
if(MCE_GPU_TESTS)
    add_test(NAME test_render_gl COMMAND test_render_gl)
    set_tests_properties(test_render_gl PROPERTIES LABELS gpu)
endif()
# The same probes on the SDL3 GPU backend (ADR-0038; CI: Vulkan through Mesa lavapipe).
if(MCE_GPU_TESTS AND MCE_RHI_SDLGPU)
    add_test(NAME test_render_sdlgpu COMMAND test_render_gl)
    set_tests_properties(test_render_sdlgpu PROPERTIES LABELS gpu ENVIRONMENT "MCE_RHI=sdlgpu")
endif()

# Golden images (FR-DBG-020): dev-machine-only, so built but never registered with ctest.
# scripts/golden.sh runs it; goldens live in golden/<os>-<backend>/.
# UMBRELLA: the cook (PNG in and out) shares the engine library.
mce_add_test(test_golden_gl UMBRELLA NO_CTEST
    SOURCES test_golden.cpp
    DEPS mce::mce_render mce::mce_rhi_default mce::mce_asset_cook)
target_compile_definitions(test_golden_gl PRIVATE MCE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
                                                  MCE_GOLDEN_OUT="${CMAKE_CURRENT_BINARY_DIR}/golden_out")
