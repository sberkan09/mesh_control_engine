cd /work/repo
B=build/linux-gcc-release
(cmake --preset linux-gcc-release -DMCE_GPU_TESTS=ON && cmake --build $B -j32 2>&1 | tail -5) 2>&1 | tail -8
echo "== gpu test list"; ctest --test-dir $B -N -L gpu 2>&1 | tail -n +1 | grep -E "Test +#|Total" | head -80
echo "== probe: offscreen GL"
SDL_VIDEODRIVER=offscreen timeout 120 ctest --test-dir $B -R '^test_rhi_gl$' -V 2>&1 | grep -i -E "renderer|vendor|opengl|nvidia|llvmpipe|passed|failed|error|No such|Test #" | head -15
echo "== probe: xvfb GL"
timeout 120 xvfb-run -a -s "-screen 0 1280x1024x24" ctest --test-dir $B -R '^test_rhi_gl$' -V 2>&1 | grep -i -E "renderer|vendor|nvidia|llvmpipe|passed|failed|error|Test #" | head -15
echo "== probe: vulkan nvidia (sdlgpu)"
VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json SDL_VIDEODRIVER=offscreen timeout 120 ctest --test-dir $B -R '^test_rhi_sdlgpu$' -V 2>&1 | grep -i -E "device|nvidia|llvmpipe|lavapipe|passed|failed|error|Test #" | head -15
VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json timeout 120 xvfb-run -a -s "-screen 0 1280x1024x24" ctest --test-dir $B -R '^test_rhi_sdlgpu$' -V 2>&1 | grep -i -E "device|nvidia|llvmpipe|lavapipe|passed|failed|error|Test #" | head -15
