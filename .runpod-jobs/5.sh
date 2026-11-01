cd /work/repo
B=build/linux-gcc-release
export XDG_RUNTIME_DIR=/tmp/xdg; mkdir -p $XDG_RUNTIME_DIR; chmod 700 $XDG_RUNTIME_DIR
export VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json
echo "== PASS A: GL (NVIDIA, SDL offscreen), ctest -L gpu"
SDL_VIDEODRIVER=offscreen timeout 2400 ctest --test-dir $B -L gpu -j6 --timeout 300 --output-on-failure > /work/gl_gpu.log 2>&1
grep -E "tests passed|tests failed|Test +#[0-9]+: .*(\*\*\*|Failed|Timeout|Not Run)" /work/gl_gpu.log | head -80
echo "== failed list"; sed -n '/The following tests FAILED/,$p' /work/gl_gpu.log | head -60
