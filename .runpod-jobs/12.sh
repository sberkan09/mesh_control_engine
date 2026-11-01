cd /work/repo
B=build/linux-gcc-release
export XDG_RUNTIME_DIR=/tmp/xdg VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json
cmake --build $B --target test_golden_gl sample_06_showcase -j32 2>&1 | tail -2
G=$B/tests/render_gl/test_golden_gl
echo "== golden GL (NVIDIA offscreen) x3"
for i in 1 2 3; do SDL_VIDEODRIVER=offscreen MCE_RHI=gl $G 2>&1 | grep -E "test cases|assertions|MISMATCH|mismatch|FAILED|ERROR: " | head -6 | cut -c1-200; done
echo "== golden sdlgpu (NVIDIA Vulkan, xvfb) x3"
for i in 1 2 3; do MCE_RHI=sdlgpu xvfb-run -a -s "-screen 0 1920x1080x24" $G 2>&1 | grep -E "test cases|assertions|MISMATCH|mismatch|FAILED|ERROR: " | head -6 | cut -c1-200; done
echo "== 06_showcase sheet 1080p, GL NVIDIA"
S=$B/samples/06_showcase/sample_06_showcase
SDL_VIDEODRIVER=offscreen timeout 600 $S -sheet /work/sheet_gl.ppm -sheet.w 1920 -sheet.h 1080 -platform.error_box 0 -r.vsync 0 2>&1 | tail -34 | cut -c1-160
echo "== 06_showcase sheet 1080p, sdlgpu NVIDIA"
MCE_RHI=sdlgpu timeout 600 xvfb-run -a -s "-screen 0 1920x1080x24" $S -sheet /work/sheet_vk.ppm -sheet.w 1920 -sheet.h 1080 -platform.error_box 0 -r.vsync 0 2>&1 | tail -34 | cut -c1-160
